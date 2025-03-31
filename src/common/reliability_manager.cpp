// src/common/reliability_manager.cpp

#include "reliability_manager.hpp"
#include "lumen_header.hpp"
#include "lumen_packet.hpp"
#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/system/detail/error_category.hpp>
#include <boost/system/detail/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <vector>

#include "configs.hpp"

ReliabilityManager::ReliabilityManager(boost::asio::io_context &io_context,
                                       bool send_acks, bool expect_acks)
    : retransmit_timer_(io_context, RELIABILITY_CHECK_INTERVAL),
      cleanup_timer_(io_context, CLEANUP_INTERVAL), running_(false),
      send_acks_(send_acks), expect_acks_(expect_acks) {

  // Initialize received sequence data
  {
    std::lock_guard<std::mutex> lock(received_sequences_mutex_);
    received_sequences_.clear();
  }

  // Initialize NAK tracking
  {
    std::lock_guard<std::mutex> lock(recent_naks_mutex_);
    recent_naks_.clear();
  }
}

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
  }

  {
    std::lock_guard<std::mutex> nak_lock(recent_naks_mutex_);
    recent_naks_.clear();
  }

  // Start the retransmission timer
  handle_retransmission_timer();

  // Start the cleanup timer
  handle_cleanup_timer();

  std::cout << "[RELIABILITY] Manager started with send_acks="
            << (send_acks_ ? "true" : "false")
            << ", expect_acks=" << (expect_acks_ ? "true" : "false")
            << std::endl;
}

void ReliabilityManager::stop() {
  if (!running_) {
    return;
  }

  running_ = false;

  // cancel timers
  boost::system::error_code ec;
  retransmit_timer_.cancel(ec);
  cleanup_timer_.cancel(ec);

  std::cout << "[RELIABILITY] Manager stopped." << std::endl;
}

void ReliabilityManager::add_send_packet(uint8_t seq, const LumenPacket &packet,
                                         const udp::endpoint &recipient) {
  if (!running_) {
    return;
  }

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  // Store the packet for potential retransmission
  sent_packets_.emplace(
      seq,
      SentPacketInfo{packet, std::chrono::steady_clock::now(), 0, recipient});

  std::cout << "[RELIABILITY] Tracking packet with seq: "
            << static_cast<int>(seq) << std::endl;
}

void ReliabilityManager::process_ack(uint8_t seq) {
  if (!running_ || !expect_acks_) {
    return;
  }

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  // Remove the acknowledged packet from tracking
  auto it = sent_packets_.find(seq);
  if (it != sent_packets_.end()) {
    sent_packets_.erase(it);
    std::cout << "[RELIABILITY] Received ACK for seq: " << static_cast<int>(seq)
              << ", removed from retransmission tracking" << std::endl;

    // Check if there are any packets with older sequence numbers that should be
    // considered delivered This handles the case where ACKs might be lost but
    // later packets arrive
    std::vector<uint8_t> to_remove;

    for (const auto &[old_seq, info] : sent_packets_) {
      // If this sequence is older than the ACKed one (considering wraparound)
      // and it's been at least 5 seconds since transmission, assume it was
      // delivered
      if (((seq - old_seq) & 0xFF) < 128) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - info.sent_time);

        if (elapsed.count() >= 5) {
          to_remove.push_back(old_seq);
        }
      }
    }

    // Remove any packets we've determined are likely delivered
    for (uint8_t old_seq : to_remove) {
      sent_packets_.erase(old_seq);
      std::cout << "[RELIABILITY] Implicitly ACKing old seq: "
                << static_cast<int>(old_seq) << " due to newer ACK"
                << std::endl;
    }
  } else {
    std::cout << "[RELIABILITY] Received ACK for seq: " << static_cast<int>(seq)
              << ", but packet not in tracking (already ACKed or never sent)"
              << std::endl;
  }
}

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

void ReliabilityManager::record_received_sequence(uint8_t seq) {
  if (!running_) {
    return;
  }

  std::lock_guard<std::mutex> lock(received_sequences_mutex_);

  // Record this sequence with current timestamp
  received_sequences_[seq] = std::chrono::steady_clock::now();
}

std::vector<uint8_t>
ReliabilityManager::get_missing_sequences(uint8_t current_seq,
                                          uint8_t window_size) {
  std::lock_guard<std::mutex> lock(received_sequences_mutex_);

  std::vector<uint8_t> missing_seqs;

  // Only look backward from current sequence, not forward
  // This is critical - we're looking for packets we should have already
  // received Window size should be small to avoid requesting old packets
  const uint8_t MAX_LOOKBACK = 16; // Only look back 16 packets max
  uint8_t actual_window = std::min(window_size, MAX_LOOKBACK);

  // Start looking from (current_seq - 1) down to (current_seq - actual_window)
  for (uint8_t i = 1; i <= actual_window; i++) {
    // Handle 8-bit wraparound correctly
    uint8_t seq_to_check = (current_seq - i) & 0xFF;

    // Don't request NAKs for sequence 0, which might not have been sent
    if (seq_to_check == 0)
      continue;

    // Check if we missed this sequence
    if (received_sequences_.find(seq_to_check) == received_sequences_.end()) {
      // Check if we've seen a later sequence for at least 100ms to allow for
      // jitter
      auto now = std::chrono::steady_clock::now();
      bool have_later_seq = false;

      // Look for any sequence between the missing one and current
      for (uint8_t j = 1; j < i; j++) {
        uint8_t later_seq = (seq_to_check + j) & 0xFF;
        auto it = received_sequences_.find(later_seq);
        if (it != received_sequences_.end()) {
          auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
              now - it->second);
          if (elapsed.count() >= 100) { // 100ms jitter allowance
            have_later_seq = true;
            break;
          }
        }
      }

      if (have_later_seq) {
        missing_seqs.push_back(seq_to_check);
      }
    }
  }

  return missing_seqs;
}

bool ReliabilityManager::is_recently_naked(uint8_t seq) {
  std::lock_guard<std::mutex> lock(recent_naks_mutex_);

  auto it = recent_naks_.find(seq);
  if (it == recent_naks_.end()) {
    return false;
  }

  // Check if we've sent a NAK for this sequence recently
  auto now = std::chrono::steady_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);

  return elapsed < NAK_DEBOUNCE_TIME;
}

void ReliabilityManager::record_nak_sent(uint8_t seq) {
  std::lock_guard<std::mutex> lock(recent_naks_mutex_);
  recent_naks_[seq] = std::chrono::steady_clock::now();
}

bool ReliabilityManager::is_sequence_in_window(uint8_t seq,
                                               uint8_t window_start,
                                               uint8_t window_size) const {
  for (uint8_t i = 0; i < window_size; i++) {
    if (seq == static_cast<uint8_t>(window_start + i)) {
      return true;
    }
  }
  return false;
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
      // Only retry if we're expecting ACKs (i.e., we're in ROVER mode)
      // or we're in BASE_STATION mode (which expects NAKs, but we still need
      // timeouts)
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

void ReliabilityManager::cleanup_old_entries() {
  auto now = std::chrono::steady_clock::now();

  // Clean up old received sequences
  {
    std::lock_guard<std::mutex> lock(received_sequences_mutex_);
    for (auto it = received_sequences_.begin();
         it != received_sequences_.end();) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - it->second);
      if (elapsed > SEQUENCE_RETAIN_TIME) {
        it = received_sequences_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Clean up old NAK records
  {
    std::lock_guard<std::mutex> lock(recent_naks_mutex_);
    for (auto it = recent_naks_.begin(); it != recent_naks_.end();) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - it->second);
      if (elapsed > NAK_DEBOUNCE_TIME * 2) { // Keep for twice the debounce time
        it = recent_naks_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void ReliabilityManager::handle_cleanup_timer() {
  if (!running_) {
    return;
  }

  cleanup_old_entries();

  // Reschedule the timer
  if (running_) {
    cleanup_timer_.expires_at(cleanup_timer_.expiry() + CLEANUP_INTERVAL);
    cleanup_timer_.async_wait([this](const boost::system::error_code &error) {
      if (!error && running_) {
        handle_cleanup_timer();
      }
    });
  }
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