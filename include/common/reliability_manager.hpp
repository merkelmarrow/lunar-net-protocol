// include/common/reliability_manager.hpp
#pragma once

#include "configs.hpp"
#include "lumen_packet.hpp"
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using boost::asio::ip::udp;

// manages the reliability aspects of the lumen protocol based on role
class ReliabilityManager {
public:
  // defines the operational role, determining the reliability strategy
  enum class Role {
    BASE_STATION, // sends acks, retransmits on nak
    ROVER         // expects acks, sends naks, retransmits on timeout
  };

  ReliabilityManager(boost::asio::io_context &io_context, bool is_base_station);

  ~ReliabilityManager();

  ReliabilityManager(const ReliabilityManager &) = delete;
  ReliabilityManager &operator=(const ReliabilityManager &) = delete;

  void start();

  void stop();

  // adds information about a sent packet for reliability tracking
  void add_send_packet(uint8_t seq, const LumenPacket &packet,
                       const udp::endpoint &recipient);

  // processes a received ack packet (rover role only)
  void process_ack(uint8_t seq);

  // processes a received nak packet (base station role only)
  void process_nak(uint8_t seq);

  using TimeoutCallback = std::function<void(const udp::endpoint &recipient)>;
  void set_timeout_callback(TimeoutCallback callback);

  // records that a packet with a specific sequence number was received from a
  // sender
  void record_received_sequence(uint8_t seq, const udp::endpoint &sender);

  // identifies potentially missing sequence numbers based on received history
  // (rover role only)
  std::vector<uint8_t> get_missing_sequences(const udp::endpoint &sender);

  // checks if a nak was sent for a specific sequence within the debounce
  // interval (rover role only)
  bool is_recently_naked(uint8_t seq);

  // records the time when a nak was sent for a specific sequence (rover role
  // only)
  void record_nak_sent(uint8_t seq);

  // gets a list of packets that have timed out and need retransmission (rover
  // role only)
  std::vector<std::pair<LumenPacket, udp::endpoint>>
  get_packets_to_retransmit();

  // sets the callback function used to trigger packet retransmissions
  void set_retransmit_callback(
      std::function<void(const LumenPacket &, const udp::endpoint &)> callback);

  // checks if the base station has already sent an ack for a given sequence
  // from an endpoint
  bool has_acked_sequence(uint8_t seq, const udp::endpoint &endpoint);

  // records that the base station has sent an ack for a given sequence from an
  // endpoint
  void record_acked_sequence(uint8_t seq, const udp::endpoint &endpoint);

  void reset_state();

private:
  // stores details about a sent packet needed for reliability tracking
  struct SentPacketInfo {
    LumenPacket packet;
    std::chrono::steady_clock::time_point sent_time;
    int retry_count;
    udp::endpoint recipient;

    SentPacketInfo(const LumenPacket &p,
                   std::chrono::steady_clock::time_point t, int r,
                   const udp::endpoint &e)
        : packet(p), sent_time(t), retry_count(r), recipient(e) {}
  };

  // handler for the periodic retransmission timer
  void handle_retransmission_timer();

  // handler for the periodic cleanup timer
  void handle_cleanup_timer();

  // removes old entries from tracking maps
  void cleanup_old_entries();

  // --- member variables ---

  // stores packets sent awaiting ack (rover) or potential nak (base)
  std::map<uint8_t, SentPacketInfo> sent_packets_;

  // stores sequence numbers acked by the base station, per endpoint key
  std::unordered_map<std::string, std::set<uint8_t>> acked_sequences_;
  std::mutex acked_sequences_mutex_;

  // stores timestamps of received sequences, per endpoint key
  std::unordered_map<std::string,
                     std::map<uint8_t, std::chrono::steady_clock::time_point>>
      received_sequences_;

  // stores timestamps of recently sent naks (rover)
  std::map<uint8_t, std::chrono::steady_clock::time_point> recent_naks_;

  // timer for periodic retransmission checks (rover)
  boost::asio::steady_timer retransmit_timer_;
  // timer for periodic cleanup of old tracking data
  boost::asio::steady_timer cleanup_timer_;

  // callback function (set by lumenprotocol) to request packet retransmission
  std::function<void(const LumenPacket &, const udp::endpoint &)>
      retransmit_callback_;

  std::mutex sent_packets_mutex_;
  std::mutex received_sequences_mutex_;
  std::mutex recent_naks_mutex_;
  std::mutex callback_mutex_;

  std::atomic<bool> running_;
  Role role_;

  std::chrono::steady_clock::time_point last_ack_cleanup_time_{};

  static constexpr uint8_t WINDOW_SIZE = 16;

  // callback for max retry notification
  TimeoutCallback timeout_callback_ = nullptr;
  std::mutex timeout_callback_mutex_;
  // counter for consecutive timeouts to base (rover role)
  int consecutive_timeouts_to_base_ = 0;
};