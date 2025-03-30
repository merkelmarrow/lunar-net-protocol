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