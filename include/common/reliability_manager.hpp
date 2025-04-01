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
  enum class Role {
    BASE_STATION, // Sends ACKs, expects NAKs
    ROVER         // Sends NAKs, expects ACKs
  };

  ReliabilityManager(boost::asio::io_context &io_context, bool is_base_station);
  ~ReliabilityManager();

  void start();
  void stop();

  // Track sent packages for possible retransmission
  void add_send_packet(uint8_t seq, const LumenPacket &packet,
                       const udp::endpoint &recipient);

  // Process acknowledgements
  void process_ack(uint8_t seq);
  void process_nak(uint8_t seq);

  // Record received sequences
  void record_received_sequence(uint8_t seq, const udp::endpoint &sender);

  // Check for missing sequences in the last window_size packets
  std::vector<uint8_t> get_missing_sequences(const udp::endpoint &sender);

  // NAK tracking to avoid duplicate NAKs
  bool is_recently_naked(uint8_t seq);
  void record_nak_sent(uint8_t seq);

  // Get messages that need retransmission
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
  void handle_cleanup_timer();
  void cleanup_old_entries();

  // Map of sequence numbers to packet info
  std::map<uint8_t, SentPacketInfo> sent_packets_;

  // Track received sequences by endpoint
  std::unordered_map<std::string,
                     std::map<uint8_t, std::chrono::steady_clock::time_point>>
      received_sequences_;

  // Track recently sent NAKs
  std::map<uint8_t, std::chrono::steady_clock::time_point> recent_naks_;

  boost::asio::steady_timer retransmit_timer_;
  boost::asio::steady_timer cleanup_timer_;

  // Retransmission callback
  std::function<void(const LumenPacket &, const udp::endpoint &)>
      retransmit_callback_;

  std::mutex sent_packets_mutex_;
  std::mutex received_sequences_mutex_;
  std::mutex recent_naks_mutex_;
  std::mutex callback_mutex_;

  bool running_;
  Role role_;

  // Constants
  static constexpr uint8_t WINDOW_SIZE = 16;
  static constexpr std::chrono::milliseconds NAK_DEBOUNCE_TIME{500};
  static constexpr std::chrono::milliseconds CLEANUP_INTERVAL{10000};
  static constexpr std::chrono::milliseconds SEQUENCE_RETAIN_TIME{30000};
};