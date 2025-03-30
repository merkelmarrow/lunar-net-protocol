// src/common/reliability_manager.cpp

#include "reliability_manager.hpp"
#include "lumen_packet.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/system/detail/error_category.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>

ReliabilityManager::ReliabilityManager(boost::asio::io_context &io_context)
    : next_expected_sequence_(0), retransmit_timer_(io_context, CHECK_INTERVAL),
      running_(false) {}

ReliabilityManager::~ReliabilityManager() { stop(); }

void ReliabilityManager::start() {
  if (running_) {
    return;
  }

  running_ = true;

  // clear data structures before starting
  {
    std::lock_guard<std::mutex> sent_lock(sent_packets_mutex_);
    sent_packets_.clear();
  }

  {
    std::lock_guard<std::mutex> recv_lock(received_sequences_mutex_);
    received_sequences_.clear();
    next_expected_sequence_ = 0;
  }

  handle_retransmission_timer();

  std::cout << "[RELIABILITY] Manager started." << std::endl;
}

void ReliabilityManager::stop() {
  if (!running_) {
    return;
  }

  running_ = false;

  // cancel timer
  boost::system::error_code ec;
  retransmit_timer_.cancel(ec);

  std::cout << "[RELIABILITY] Manager stopped." << std::endl;
}

void ReliabilityManager::add_send_packet(uint8_t seq, const LumenPacket &packet,
                                         const udp::endpoint &recipient) {
  if (!running_) {
    return;
  }

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  sent_packets_.emplace(
      seq,
      SentPacketInfo{packet, std::chrono::steady_clock::now(), 0, recipient});

  std::cout << "[RELIABILITY] Tracking packet with seq: "
            << static_cast<int>(seq) << std::endl;
}

void ReliabilityManager::process_ack(uint8_t seq) {
  if (!running_) {
    return;
  }

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  // remove the acknowledged packet from tracking
  auto it = sent_packets_.find(seq);
  if (it != sent_packets_.end()) {
    sent_packets_.erase(it);
    std::cout << "[RELIABILITY] Received ACK for seq: " << static_cast<int>(seq)
              << std::endl;
  }
}

// process SACKs
void ReliabilityManager::process_sack(
    const std::vector<uint8_t> &missing_seqs) {
  if (!running_) {
    return;
  }

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  std::cout << "[RELIABILITY] Received SACK with " << missing_seqs.size()
            << " missing sequences." << std::endl;

  for (uint8_t seq : missing_seqs) {
    auto it = sent_packets_.find(seq);
    if (it != sent_packets_.end()) {
      // get a copy of retransmit callback for thread safety
      std::function<void(const LumenPacket &, const udp::endpoint &)>
          callback_copy;
      {
        std::lock_guard<std::mutex> callback_lock(callback_mutex_);
        callback_copy = retransmit_callback_;
      }

      if (callback_copy) {
        // reset retry counter (SACK is a special case)
        it->second.retry_count = 0;

        // update sent time
        it->second.sent_time = std::chrono::steady_clock::now();

        // retransmit
        callback_copy(it->second.packet, it->second.recipient);
        std::cout << "[RELIABILITY] Retransmitting packet with seq: "
                  << static_cast<int>(seq) << " (SACK)" << std::endl;
      }
    }
  }
}