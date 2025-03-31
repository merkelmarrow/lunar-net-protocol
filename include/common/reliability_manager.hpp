// include/common/reliability_manager.hpp

#pragma once

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
#include <unordered_map>
#include <utility>
#include <vector>

using boost::asio::ip::udp;

class ReliabilityManager {
public:
  ReliabilityManager(boost::asio::io_context &io_context,
                     bool send_acks = false, bool expect_acks = false);
  ~ReliabilityManager();

  void start();
  void stop();

  // track sent packages for possible retransmission
  void add_send_packet(uint8_t seq, const LumenPacket &packet,
                       const udp::endpoint &recipient);

  // process acknowledgements
  void process_ack(uint8_t seq);
  void process_nak(uint8_t seq);

  // record received sequences for NAK generation
  void record_received_sequence(uint8_t seq);

  // Get missing sequence numbers in the window
  std::vector<uint8_t> get_missing_sequences(uint8_t current_seq,
                                             uint8_t window_size);

  // NAK tracking to avoid duplicate NAKs
  bool is_recently_naked(uint8_t seq);
  void record_nak_sent(uint8_t seq);

  // get messages that need retransmission
  std::vector<std::pair<LumenPacket, udp::endpoint>>
  get_packets_to_retransmit();

  void set_retransmit_callback(
      std::function<void(const LumenPacket &, const udp::endpoint &)> callback);

private:
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

  void handle_retransmission_timer();

  // map of sequence numbers to packet info
  std::map<uint8_t, SentPacketInfo> sent_packets_;

  // track received sequence numbers with timestamps for jitter handling
  std::map<uint8_t, std::chrono::steady_clock::time_point> received_sequences_;

  // track recently sent NAKs with timestamps
  std::map<uint8_t, std::chrono::steady_clock::time_point> recent_naks_;

  boost::asio::steady_timer retransmit_timer_;
  boost::asio::steady_timer cleanup_timer_;

  // retransmission callback
  std::function<void(const LumenPacket &, const udp::endpoint &)>
      retransmit_callback_;

  std::mutex sent_packets_mutex_;
  std::mutex received_sequences_mutex_;
  std::mutex recent_naks_mutex_;
  std::mutex callback_mutex_;

  // helper to check if a sequence number is within a window, accounting for
  // wraparound
  bool is_sequence_in_window(uint8_t seq, uint8_t window_start,
                             uint8_t window_size) const;

  // Clean up old entries in tracking maps
  void cleanup_old_entries();
  void handle_cleanup_timer();

  bool running_;
  bool send_acks_;   // Base station mode: true, Rover mode: false
  bool expect_acks_; // Base station mode: false, Rover mode: true

  // Constants
  static constexpr std::chrono::milliseconds NAK_DEBOUNCE_TIME{
      500}; // Don't send NAKs too frequently
  static constexpr std::chrono::milliseconds CLEANUP_INTERVAL{
      5000}; // Clean up old tracking entries every 5 seconds
  static constexpr std::chrono::milliseconds SEQUENCE_RETAIN_TIME{
      10000}; // Keep received sequences for 10 seconds
};