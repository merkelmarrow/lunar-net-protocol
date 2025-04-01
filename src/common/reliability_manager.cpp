// src/common/reliability_manager.cpp
#include "reliability_manager.hpp"
#include <iostream>
#include <sstream>

// Utility function to get endpoint key
std::string get_endpoint_key(const udp::endpoint &endpoint) {
  std::stringstream ss;
  ss << endpoint.address().to_string() << ":" << endpoint.port();
  return ss.str();
}

ReliabilityManager::ReliabilityManager(boost::asio::io_context &io_context,
                                       bool is_base_station)
    : retransmit_timer_(io_context, RELIABILITY_CHECK_INTERVAL),
      cleanup_timer_(io_context, CLEANUP_INTERVAL), running_(false),
      role_(is_base_station ? Role::BASE_STATION : Role::ROVER) {

  std::cout << "[RELIABILITY] Manager created with role: "
            << (is_base_station ? "BASE_STATION" : "ROVER") << std::endl;
}

ReliabilityManager::~ReliabilityManager() { stop(); }

void ReliabilityManager::start() {
  if (running_) {
    return;
  }

  running_ = true;

  // Clear all data structures
  {
    std::lock_guard<std::mutex> lock(sent_packets_mutex_);
    sent_packets_.clear();
  }

  {
    std::lock_guard<std::mutex> lock(received_sequences_mutex_);
    received_sequences_.clear();
  }

  {
    std::lock_guard<std::mutex> lock(recent_naks_mutex_);
    recent_naks_.clear();
  }

  // Start timers
  handle_retransmission_timer();
  handle_cleanup_timer();

  std::cout << "[RELIABILITY] Manager started in "
            << (role_ == Role::BASE_STATION ? "BASE_STATION" : "ROVER")
            << " mode" << std::endl;
}

void ReliabilityManager::stop() {
  if (!running_) {
    return;
  }

  running_ = false;

  // Cancel timers
  boost::system::error_code ec;
  retransmit_timer_.cancel(ec);
  cleanup_timer_.cancel(ec);

  std::cout << "[RELIABILITY] Manager stopped" << std::endl;
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
  if (!running_ || role_ != Role::ROVER) {
    return;
  }

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  // Remove the acknowledged packet from tracking
  auto it = sent_packets_.find(seq);
  if (it != sent_packets_.end()) {
    sent_packets_.erase(it);
    std::cout << "[RELIABILITY] Received ACK for seq: " << static_cast<int>(seq)
              << ", removed from retransmission tracking" << std::endl;
  } else {
    std::cout << "[RELIABILITY] Received ACK for seq: " << static_cast<int>(seq)
              << ", but packet not in tracking (already ACKed or never sent)"
              << std::endl;
  }
}

void ReliabilityManager::process_nak(uint8_t seq) {
  if (!running_ || role_ != Role::BASE_STATION) {
    return;
  }

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  // Find the requested packet in our sent packets
  auto it = sent_packets_.find(seq);
  if (it != sent_packets_.end()) {
    // Get a copy of retransmit callback
    std::function<void(const LumenPacket &, const udp::endpoint &)>
        callback_copy;
    {
      std::lock_guard<std::mutex> callback_lock(callback_mutex_);
      callback_copy = retransmit_callback_;
    }

    if (callback_copy) {
      // Reset retransmission timer to avoid immediate retries
      it->second.sent_time = std::chrono::steady_clock::now();

      // Retransmit the packet
      callback_copy(it->second.packet, it->second.recipient);
      std::cout << "[RELIABILITY] Retransmitting packet with seq: "
                << static_cast<int>(seq) << " (NAK)" << std::endl;
    }
  } else {
    std::cout << "[RELIABILITY] Received NAK for unknown sequence: "
              << static_cast<int>(seq) << std::endl;
  }
}

void ReliabilityManager::record_received_sequence(uint8_t seq,
                                                  const udp::endpoint &sender) {
  if (!running_) {
    return;
  }

  std::string endpoint_key = get_endpoint_key(sender);

  std::lock_guard<std::mutex> lock(received_sequences_mutex_);
  received_sequences_[endpoint_key][seq] = std::chrono::steady_clock::now();
}

std::vector<uint8_t>
ReliabilityManager::get_missing_sequences(const udp::endpoint &sender) {
  if (role_ != Role::ROVER) {
    return {};
  }

  std::string endpoint_key = get_endpoint_key(sender);
  std::lock_guard<std::mutex> lock(received_sequences_mutex_);

  if (received_sequences_.find(endpoint_key) == received_sequences_.end()) {
    return {};
  }

  auto &sequence_map = received_sequences_[endpoint_key];
  std::vector<uint8_t> missing_seqs;

  if (sequence_map.empty()) {
    return missing_seqs;
  }

  // Find the highest sequence number
  uint8_t highest_seq = sequence_map.rbegin()->first;

  // Only look for gaps in a reasonable window size
  for (uint8_t i = 1; i <= WINDOW_SIZE; i++) {
    // Compute sequence number with wraparound
    uint8_t seq_to_check = (highest_seq - i) & 0xFF;

    // Skip sequence 0 which might not exist
    if (seq_to_check == 0)
      continue;

    // If we haven't received this sequence and it's within a reasonable range
    if (sequence_map.find(seq_to_check) == sequence_map.end()) {
      // Make sure we've seen a later sequence number for at least 200ms
      // to account for out-of-order delivery
      bool have_later_seq = false;
      auto now = std::chrono::steady_clock::now();

      for (uint8_t j = 1; j < i; j++) {
        uint8_t later_seq = (seq_to_check + j) & 0xFF;
        auto it = sequence_map.find(later_seq);
        if (it != sequence_map.end()) {
          auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
              now - it->second);
          if (elapsed.count() >= 200) {
            have_later_seq = true;
            break;
          }
        }
      }

      // Only request NAK if we're confident it's actually missing
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

  auto now = std::chrono::steady_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);

  return elapsed < NAK_DEBOUNCE_TIME;
}

void ReliabilityManager::record_nak_sent(uint8_t seq) {
  std::lock_guard<std::mutex> lock(recent_naks_mutex_);
  recent_naks_[seq] = std::chrono::steady_clock::now();
}

std::vector<std::pair<LumenPacket, udp::endpoint>>
ReliabilityManager::get_packets_to_retransmit() {
  std::lock_guard<std::mutex> lock(sent_packets_mutex_);
  std::vector<std::pair<LumenPacket, udp::endpoint>> packets_to_retransmit;

  auto now = std::chrono::steady_clock::now();

  for (auto it = sent_packets_.begin(); it != sent_packets_.end();) {
    auto &info = it->second;
    uint8_t seq = it->first;

    // Skip packets we've already ACKed (for base station)
    if (role_ == Role::BASE_STATION &&
        has_acked_sequence(seq, info.recipient)) {
      std::cout << "[RELIABILITY] Removing packet with seq: "
                << static_cast<int>(seq) << " that we've already ACKed"
                << std::endl;
      it = sent_packets_.erase(it);
      continue;
    }

    // Rest of the function remains the same
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - info.sent_time);

    auto timeout = RELIABILITY_BASE_TIMEOUT * (1 << info.retry_count);

    if (elapsed > timeout) {
      if (info.retry_count >= RELIABILITY_MAX_RETRIES) {
        std::cout << "[RELIABILITY] Max retries reached for seq: "
                  << static_cast<int>(it->first) << ", giving up." << std::endl;
        it = sent_packets_.erase(it);
        continue;
      }

      packets_to_retransmit.push_back({info.packet, info.recipient});

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

void ReliabilityManager::set_retransmit_callback(
    std::function<void(const LumenPacket &, const udp::endpoint &)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  retransmit_callback_ = std::move(callback);
}

void ReliabilityManager::handle_retransmission_timer() {
  if (!running_) {
    return;
  }

  // Get packets that need retransmission
  auto packets = get_packets_to_retransmit();

  // Get callback
  std::function<void(const LumenPacket &, const udp::endpoint &)> callback_copy;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_copy = retransmit_callback_;
  }

  // Retransmit packets
  if (callback_copy) {
    for (const auto &[packet, endpoint] : packets) {
      callback_copy(packet, endpoint);
    }
  }

  // Reschedule timer
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

void ReliabilityManager::cleanup_old_entries() {
  auto now = std::chrono::steady_clock::now();

  // Clean up received sequences
  {
    std::lock_guard<std::mutex> lock(received_sequences_mutex_);
    for (auto &[endpoint, sequences] : received_sequences_) {
      for (auto it = sequences.begin(); it != sequences.end();) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second);
        if (elapsed > SEQUENCE_RETAIN_TIME) {
          it = sequences.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  // Clean up NAK records
  {
    std::lock_guard<std::mutex> lock(recent_naks_mutex_);
    for (auto it = recent_naks_.begin(); it != recent_naks_.end();) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - it->second);
      if (elapsed > NAK_DEBOUNCE_TIME) {
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

  if (running_) {
    cleanup_timer_.expires_at(cleanup_timer_.expiry() + CLEANUP_INTERVAL);
    cleanup_timer_.async_wait([this](const boost::system::error_code &error) {
      if (!error && running_) {
        handle_cleanup_timer();
      }
    });
  }
}

// Record that we've ACKed a sequence
void ReliabilityManager::record_acked_sequence(uint8_t seq,
                                               const udp::endpoint &endpoint) {
  if (role_ != Role::BASE_STATION)
    return;

  std::string endpoint_key = get_endpoint_key(endpoint);
  std::lock_guard<std::mutex> lock(acked_sequences_mutex_);
  acked_sequences_[endpoint_key].insert(seq);
}

// Check if we've already ACKed a sequence
bool ReliabilityManager::has_acked_sequence(uint8_t seq,
                                            const udp::endpoint &endpoint) {
  if (role_ != Role::BASE_STATION)
    return false;

  std::string endpoint_key = get_endpoint_key(endpoint);
  std::lock_guard<std::mutex> lock(acked_sequences_mutex_);

  auto it = acked_sequences_.find(endpoint_key);
  if (it == acked_sequences_.end())
    return false;

  return it->second.find(seq) != it->second.end();
}