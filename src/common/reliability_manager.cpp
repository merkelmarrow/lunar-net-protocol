// src/common/reliability_manager.cpp

#include "reliability_manager.hpp"
#include "lumen_header.hpp"
#include "lumen_packet.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/system/detail/error_category.hpp>
#include <boost/system/detail/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <vector>

#include "configs.hpp"

ReliabilityManager::ReliabilityManager(boost::asio::io_context &io_context)
    : next_expected_sequence_(0),
      retransmit_timer_(io_context, RELIABILITY_CHECK_INTERVAL),
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

void ReliabilityManager::record_received_sequence(uint8_t seq) {
  if (!running_) {
    return;
  }

  std::lock_guard<std::mutex> lock(received_sequences_mutex_);

  // add to received sequences
  received_sequences_.insert(seq);

  // update the next expected sequence if this is the one we're waiting for
  if (seq == next_expected_sequence_) {
    // find the next gap
    while (received_sequences_.find(next_expected_sequence_) !=
           received_sequences_.end()) {
      next_expected_sequence_ =
          (next_expected_sequence_ + 1) & 0xFF; // wrapping
    }
  }
}

LumenPacket
ReliabilityManager::generate_sack_packet(uint8_t next_expected_seq) {
  std::lock_guard<std::mutex> lock(received_sequences_mutex_);

  // find missing sequences in the window
  std::vector<uint8_t> missing_seqs;

  // look for gaps in a window of sack window sequences
  for (uint8_t i = 0; i < SACK_WINDOW_SIZE; i++) {
    uint8_t seq =
        static_cast<uint8_t>(next_expected_seq - SACK_WINDOW_SIZE + i);

    if (received_sequences_.find(seq) == received_sequences_.end()) {
      missing_seqs.push_back(seq);
    }
  }

  // create a sack header
  LumenHeader header(LumenHeader::MessageType::SACK,
                     LumenHeader::Priority::HIGH, next_expected_seq,
                     0, // timestamp 0 for SACK
                     static_cast<uint16_t>(missing_seqs.size()));

  return LumenPacket(header, missing_seqs);
}

// get messages that need retransmission
std::vector<std::pair<LumenPacket, udp::endpoint>>
ReliabilityManager::get_packets_to_retransmit() {
  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  std::vector<std::pair<LumenPacket, udp::endpoint>> packets_to_retransmit;

  auto now = std::chrono::steady_clock::now();

  for (auto it = sent_packets_.begin(); it != sent_packets_.end();) {
    auto &info = it->second;

    // calculate time since transmission
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - info.sent_time);

    // calculate timeout based on retry count (exponential backoff)
    auto timeout = RELIABILITY_BASE_TIMEOUT * (1 << info.retry_count);

    if (elapsed > timeout) {
      if (info.retry_count >= RELIABILITY_MAX_RETRIES) {
        std::cout << "[RELIABILITY] Max retries reached for seq: "
                  << static_cast<int>(it->first) << ", giving up." << std::endl;

        // stop tracking
        it = sent_packets_.erase(it);
        continue;
      }

      // add to retransmission list
      packets_to_retransmit.push_back({info.packet, info.recipient});

      // update sent time and retry count
      info.sent_time = now;
      info.retry_count++;

      std::cout << "[RELIABILITY] Packet with seq: "
                << static_cast<int>(it->first)
                << " needs retransmission (retry " << info.retry_count << ")"
                << std::endl;
    }
    ++it;
  }

  return packets_to_retransmit;
}

// set callback for packet retransmission
void ReliabilityManager::set_retransmit_callback(
    std::function<void(const LumenPacket &, const udp::endpoint &)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);

  retransmit_callback_ = std::move(callback);
}

// handle retransmission timer
void ReliabilityManager::handle_retransmission_timer() {
  if (!running_) {
    return;
  }

  // get packets that need retransmission
  auto packets = get_packets_to_retransmit();

  // get a copy of retransmit callback for thread safety
  std::function<void(const LumenPacket &, const udp::endpoint &)> callback_copy;

  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_copy = retransmit_callback_;
  }

  // retransmit packets
  if (callback_copy) {
    for (const auto &[packet, endpoint] : packets) {
      callback_copy(packet, endpoint);
    }
  }

  // reschedule timer
  if (running_) {
    retransmit_timer_.expires_at(retransmit_timer_.expiry() +
                                 RELIABILITY_CHECK_INTERVAL);
    retransmit_timer_.async_wait(
        [this](const boost::system::error_code &error) {
          if (!error && running_) {
            handle_retransmission_timer();
          }
        });
  }
}