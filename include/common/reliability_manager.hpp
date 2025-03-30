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
#include <set>
#include <utility>
#include <vector>

using boost::asio::ip::udp;

class ReliabilityManager {
public:
  ReliabilityManager(boost::asio::io_context &io_context);
  ~ReliabilityManager();

  void start();
  void stop();

  // track sent packages for possible retransmission
  void add_send_packet(uint8_t seq, const LumenPacket &packet,
                       const udp::endpoint &recipient);

  // process acknowledgements
  void process_ack(uint8_t seq);
  void process_sack(const std::vector<uint8_t> &missing_seqs);

  // record received sequences for SACK generation
  void record_received_sequence(uint8_t seq);

  // generate a sack packet
  LumenPacket generate_sack_packet(uint8_t next_expected_seq);

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

  // map of sequence numebers to packet info
  std::map<uint8_t, SentPacketInfo> sent_packets_;

  // track received sequence numbers for gap detection
  std::set<uint8_t> received_sequences_;
  uint8_t next_expected_sequence_;

  boost::asio::steady_timer retransmit_timer_;

  // retransmission callback
  std::function<void(const LumenPacket &, const udp::endpoint &)>
      retransmit_callback_;

  std::mutex sent_packets_mutex_;
  std::mutex received_sequences_mutex_;
  std::mutex callback_mutex_;

  bool running_;

  // configuration constants
  static constexpr int MAX_RETRIES = 5;
  static constexpr std::chrono::milliseconds BASE_TIMEOUT{5000};
  static constexpr std::chrono::milliseconds CHECK_INTERVAL{1000};
};