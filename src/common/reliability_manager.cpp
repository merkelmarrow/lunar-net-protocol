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

// process a NAK
void ReliabilityManager::process_nak(uint8_t seq) {
  if (!running_) {
    return;
  }

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  // find the requested packet in our sent packets
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
      // reset retransmission timer to avoid immediate retries
      it->second.sent_time = std::chrono::steady_clock::now();

      // retransmit the packet
      callback_copy(it->second.packet, it->second.recipient);
      std::cout << "[RELIABILITY] Retransmitting packet with seq: "
                << static_cast<int>(seq) << " (NAK)" << std::endl;
    }
  } else {
    std::cout << "[RELIABILITY] Received NAK for unknown sequence: "
              << static_cast<int>(seq) << std::endl;
  }
}

// get the next expected sequence number
uint8_t ReliabilityManager::get_next_expected_sequence() const {
  std::lock_guard<std::mutex> lock(received_sequences_mutex_);
  return next_expected_sequence_;
}

void ReliabilityManager::record_received_sequence(uint8_t seq) {
  if (!running_) {
    return;
  }

  std::lock_guard<std::mutex> lock(received_sequences_mutex_);

  // add to received sequences set
  received_sequences_.insert(seq);

  // check if this is the next expected sequence
  if (seq == next_expected_sequence_) {
    // find the next gap in sequence numbers
    uint8_t temp = next_expected_sequence_;
    do {
      temp = (temp + 1) & 0xFF; // increment with wrap-around
    } while (received_sequences_.find(temp) != received_sequences_.end());

    next_expected_sequence_ = temp;

    // clean up received_sequences_ by removing acknowledged sequences
    auto it = received_sequences_.begin();
    while (it != received_sequences_.end()) {
      uint8_t received_seq = *it;

      // check if this sequence is before next_expected_sequence_
      bool is_before_next = false;
      if (next_expected_sequence_ > received_seq) {
        // no wrap-around
        if (next_expected_sequence_ - received_seq < 128) {
          is_before_next = true;
        }
      } else if (next_expected_sequence_ < received_seq) {
        // wrap-around
        if (received_seq - next_expected_sequence_ > 128) {
          is_before_next = true;
        }
      }

      if (is_before_next) {
        it = received_sequences_.erase(it);
      } else {
        ++it;
      }
    }

    std::cout << "[RELIABILITY] Updated next expected sequence to: "
              << static_cast<int>(next_expected_sequence_) << std::endl;
  }
}

LumenPacket ReliabilityManager::generate_nak_packet(uint8_t seq) {
  // Create a NAK payload with the sequence number
  std::vector<uint8_t> nak_payload = {seq};

  // Create a NAK header
  LumenHeader header(LumenHeader::MessageType::NAK, LumenHeader::Priority::HIGH,
                     0, // Use sequence 0 for NAK packets
                     0, // Use timestamp 0 for NAK packets
                     static_cast<uint16_t>(nak_payload.size()));

  return LumenPacket(header, nak_payload);
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