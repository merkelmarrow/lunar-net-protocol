// src/common/reliability_manager.cpp
#include "reliability_manager.hpp"
#include "configs.hpp" // Include configs for constants like RELIABILITY_CHECK_INTERVAL etc.
#include <iostream>
#include <sstream> // For std::stringstream

using boost::asio::ip::udp;

// Helper function to create a consistent string key from an endpoint.
std::string get_endpoint_key(const udp::endpoint &endpoint) {
  std::stringstream ss;
  ss << endpoint.address().to_string() << ":" << endpoint.port();
  return ss.str();
}

ReliabilityManager::ReliabilityManager(boost::asio::io_context &io_context,
                                       bool is_base_station)
    : // Initialize timers with their respective intervals
      retransmit_timer_(io_context, RELIABILITY_CHECK_INTERVAL),
      cleanup_timer_(io_context, CLEANUP_INTERVAL), running_(false),
      role_(is_base_station ? Role::BASE_STATION : Role::ROVER),
      consecutive_timeouts_to_base_(0) {

  std::cout << "[RELIABILITY] Manager created with role: "
            << (is_base_station ? "BASE_STATION" : "ROVER") << std::endl;
}

ReliabilityManager::~ReliabilityManager() { stop(); }

void ReliabilityManager::start() {
  if (running_) {
    return;
  }
  running_ = true;

  // Clear internal state from any previous runs
  {
    std::lock_guard<std::mutex> lock(sent_packets_mutex_);
    sent_packets_.clear();
  }

  consecutive_timeouts_to_base_ = 0;

  {
    std::lock_guard<std::mutex> lock(received_sequences_mutex_);
    received_sequences_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(recent_naks_mutex_);
    recent_naks_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(acked_sequences_mutex_);
    acked_sequences_.clear();
  }

  // Schedule the first checks for retransmissions and cleanup
  handle_retransmission_timer();
  handle_cleanup_timer();

  std::cout << "[RELIABILITY] Manager started in "
            << (role_ == Role::BASE_STATION ? "BASE_STATION" : "ROVER")
            << " mode." << std::endl;
}

void ReliabilityManager::stop() {
  if (!running_) {
    return;
  }
  running_ = false;

  // Cancel any pending asynchronous operations on the timers
  boost::system::error_code ec;
  retransmit_timer_.cancel(ec);
  // Ignore error codes on cancel, as it's expected if timer wasn't active
  cleanup_timer_.cancel(ec);

  std::cout << "[RELIABILITY] Manager stopped." << std::endl;
}

void ReliabilityManager::add_send_packet(uint8_t seq, const LumenPacket &packet,
                                         const udp::endpoint &recipient) {
  if (!running_) {
    return;
  }

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  // Store packet details for potential retransmission or NAK handling.
  // Use emplace for efficiency.
  sent_packets_.emplace(
      seq,
      SentPacketInfo{packet, std::chrono::steady_clock::now(), 0, recipient});
}

void ReliabilityManager::process_ack(uint8_t seq) {
  // ACKs are only processed by the Rover
  if (!running_ || role_ != Role::ROVER) {
    return;
  }

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  auto it = sent_packets_.find(seq);
  if (it != sent_packets_.end()) {
    // Packet acknowledged, remove it from tracking. No need to retransmit.
    consecutive_timeouts_to_base_ = 0;
    sent_packets_.erase(it);
  } else {
    // This can happen if the ACK arrives after the packet was already
    // retransmitted and subsequently ACKed, or if it's a duplicate ACK. It's
    // usually benign.
  }
}

void ReliabilityManager::process_nak(uint8_t seq) {
  // NAKs are only processed by the Base Station
  if (!running_ || role_ != Role::BASE_STATION) {
    return;
  }

  // Note: This function assumes immediate retransmission upon NAK.
  // It relies on the retransmit_callback_ being set by LumenProtocol.

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  auto it = sent_packets_.find(seq);
  if (it != sent_packets_.end()) {
    // Packet requested via NAK is found in our sent history. Retransmit it.
    std::function<void(const LumenPacket &, const udp::endpoint &)>
        callback_copy;
    {
      std::lock_guard<std::mutex> callback_lock(callback_mutex_);
      callback_copy = retransmit_callback_;
    }

    if (callback_copy) {
      std::cout << "[RELIABILITY] NAK received for seq: "
                << static_cast<int>(seq)
                << ". Triggering immediate retransmission." << std::endl;
      // Base station retransmits *only* when receiving a NAK.
      callback_copy(it->second.packet, it->second.recipient);
    } else {
      std::cerr << "[RELIABILITY] Error: NAK received for seq "
                << static_cast<int>(seq)
                << " but retransmit callback is not set!" << std::endl;
    }
  } else {
    // The requested sequence might have been sent long ago and cleaned up, or
    // never sent.
    std::cout << "[RELIABILITY] Received NAK for unknown/old sequence: "
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

  // Store the time when a sequence number was received from a specific
  // endpoint. This map is used by the Rover to detect gaps.
  received_sequences_[endpoint_key][seq] = std::chrono::steady_clock::now();
}

std::vector<uint8_t>
ReliabilityManager::get_missing_sequences(const udp::endpoint &sender) {
  // Only the Rover needs to detect missing sequences to send NAKs.
  if (role_ != Role::ROVER) {
    return {};
  }

  std::string endpoint_key = get_endpoint_key(sender);
  std::vector<uint8_t> missing_seqs;

  std::lock_guard<std::mutex> lock(received_sequences_mutex_);

  // Check if we have any record of receiving data from this sender
  auto endpoint_it = received_sequences_.find(endpoint_key);
  if (endpoint_it == received_sequences_.end() || endpoint_it->second.empty()) {
    return missing_seqs; // No data received yet, so nothing is missing.
  }

  auto &sequence_map =
      endpoint_it->second; // Map of seq -> time_received for this endpoint

  // Find the highest sequence number received from this sender so far.
  // `rbegin()` gives the last element (highest key in a std::map).
  uint8_t highest_seq = sequence_map.rbegin()->first;

  // Check backwards from the highest received sequence number within a defined
  // window. This avoids checking the entire sequence space (0-255).
  for (uint8_t i = 1; i <= WINDOW_SIZE; ++i) {
    // Calculate the sequence number to check, handling potential wraparound
    // (uint8_t arithmetic).
    uint8_t seq_to_check = highest_seq - i;

    // If the sequence number to check is *not* found in our map of received
    // sequences...
    if (sequence_map.find(seq_to_check) == sequence_map.end()) {
      // ...it *might* be missing. Add it to the list of potential missing
      // sequences.
      missing_seqs.push_back(seq_to_check);
    }
  }
  // Return the list of sequences suspected to be missing.
  // LumenProtocol will use this list and check is_recently_naked before sending
  // NAKs.
  return missing_seqs;
}

bool ReliabilityManager::is_recently_naked(uint8_t seq) {
  std::lock_guard<std::mutex> lock(recent_naks_mutex_);

  auto it = recent_naks_.find(seq);
  if (it == recent_naks_.end()) {
    // We haven't recorded sending a NAK for this sequence recently.
    return false;
  }

  // Check if the recorded NAK time is within the debounce interval.
  auto now = std::chrono::steady_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);

  return elapsed < NAK_DEBOUNCE_TIME;
}

void ReliabilityManager::record_nak_sent(uint8_t seq) {
  // Record the time when a NAK for this sequence was sent.
  std::lock_guard<std::mutex> lock(recent_naks_mutex_);
  recent_naks_[seq] = std::chrono::steady_clock::now();
}

// This function is primarily used by the Rover's retransmission timer.
std::vector<std::pair<LumenPacket, udp::endpoint>>
ReliabilityManager::get_packets_to_retransmit() {

  std::vector<std::pair<LumenPacket, udp::endpoint>> packets_to_retransmit;
  auto now = std::chrono::steady_clock::now();

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  // Iterate through packets we've sent and are tracking.
  for (auto it = sent_packets_.begin(); it != sent_packets_.end();) {
    auto &info = it->second; // Reference to SentPacketInfo
    uint8_t seq = it->first;

    // --- Role-Specific Retransmission Logic ---

    // BASE STATION: Never retransmits based on timer. Only retransmits upon
    // receiving a NAK (handled in process_nak). So, just continue iterating
    // without checking timeouts. The packet stays tracked in case a NAK arrives
    // later.
    if (role_ == Role::BASE_STATION) {
      ++it;
      continue;
    }

    // ROVER: Uses timeout-based retransmission. Checks if the time since
    // sending exceeds the calculated timeout.
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - info.sent_time);
    // Calculate exponential backoff timeout
    // Note: Ensure RELIABILITY_BASE_TIMEOUT and retry_count logic doesn't lead
    // to excessively long timeouts.
    auto timeout = RELIABILITY_BASE_TIMEOUT *
                   (1 << info.retry_count); // timeout = base * 2^retry_count

    if (elapsed > timeout) {
      // Timeout exceeded. Check if max retries have been reached.
      if (info.retry_count >= RELIABILITY_MAX_RETRIES) {
        bool timeout_to_base = false;
        if (info.recipient.port() == 9000) { // risky temporary assumption
          timeout_to_base = true;
        }

        if (timeout_to_base) {
          std::cerr << "[RELIABILITY] Max retries (" << RELIABILITY_MAX_RETRIES
                    << ") reached for packet seq: " << static_cast<int>(seq)
                    << " to Base Station. Notifying Rover to disconnect."
                    << std::endl;

          // Get callback copy safely
          TimeoutCallback callback_copy;
          {
            std::lock_guard<std::mutex> cb_lock(timeout_callback_mutex_);
            callback_copy = timeout_callback_;
          }

          if (callback_copy) {
            try {
              callback_copy(info.recipient); // Notify Rover about the timeout
            } catch (const std::exception &e) {
              std::cerr << "[RELIABILITY] Error in timeout callback: "
                        << e.what() << std::endl;
            }
          } else {
            std::cerr << "[RELIABILITY] Error: Max retries reached for base, "
                         "but timeout callback is not set!"
                      << std::endl;
          }
        } else {
          // Max retries reached for a non-base recipient
          std::cerr << "[RELIABILITY] Max retries (" << RELIABILITY_MAX_RETRIES
                    << ") reached for packet seq: " << static_cast<int>(seq)
                    << " to non-base recipient " << info.recipient
                    << ". Giving up." << std::endl;
        }
        std::cerr << "[RELIABILITY] Max retries (" << RELIABILITY_MAX_RETRIES
                  << ") reached for packet seq: " << static_cast<int>(seq)
                  << ". Giving up." << std::endl;
        // Erase the packet from tracking and continue to the next.
        it = sent_packets_.erase(it);
      } else {
        // Add packet to the list for retransmission.
        packets_to_retransmit.push_back({info.packet, info.recipient});

        // Update the packet info: reset sent_time and increment retry_count.
        info.sent_time = now;
        info.retry_count++;

        std::cout << "[RELIABILITY] Timeout detected for packet seq: "
                  << static_cast<int>(seq)
                  << ". Queued for retransmission (attempt " << info.retry_count
                  << ")." << std::endl;
        // Move to the next packet in the map.
        ++it;
      }
    } else {
      // Packet has not timed out yet. Move to the next packet.
      ++it;
    }
  } // End of loop through sent_packets_

  return packets_to_retransmit;
}

void ReliabilityManager::set_retransmit_callback(
    std::function<void(const LumenPacket &, const udp::endpoint &)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  retransmit_callback_ = std::move(callback);
}

void ReliabilityManager::set_timeout_callback(TimeoutCallback callback) {
  std::lock_guard<std::mutex> lock(timeout_callback_mutex_);
  timeout_callback_ = std::move(callback);
}

// Periodic timer handler, primarily for Rover's timeout-based retransmissions.
void ReliabilityManager::handle_retransmission_timer() {
  if (!running_) {
    return;
  }

  // This check is only relevant for the Rover.
  if (role_ == Role::ROVER) {
    auto packets = get_packets_to_retransmit(); // Get list of packets needing
                                                // retransmission

    if (!packets.empty()) {
      // Get a copy of the callback to avoid holding the lock while calling it.
      std::function<void(const LumenPacket &, const udp::endpoint &)>
          callback_copy;
      {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback_copy = retransmit_callback_;
      }

      // Trigger retransmission via the callback (set by LumenProtocol).
      if (callback_copy) {
        for (const auto &[packet, endpoint] : packets) {
          callback_copy(packet, endpoint);
        }
      } else {
        std::cerr << "[RELIABILITY] Error: Packets need retransmission, but "
                     "retransmit callback is not set!"
                  << std::endl;
      }
    }
  }
  // Base station doesn't use this timer for retransmissions, but the timer
  // still runs periodically.

  // Reschedule the timer for the next check.
  if (running_) {
    retransmit_timer_.expires_at(retransmit_timer_.expiry() +
                                 RELIABILITY_CHECK_INTERVAL);
    retransmit_timer_.async_wait(
        [this](const boost::system::error_code &error) {
          // Check error code (e.g., operation_aborted if stop() is called) and
          // running state.
          if (!error && running_) {
            handle_retransmission_timer();
          }
        });
  }
}

// Removes old entries from tracking maps to prevent them from growing
// indefinitely.
void ReliabilityManager::cleanup_old_entries() {
  auto now = std::chrono::steady_clock::now();

  // Clean up received sequences map
  {
    std::lock_guard<std::mutex> lock(received_sequences_mutex_);
    // Iterate through each endpoint's sequence map
    for (auto &pair : received_sequences_) {
      auto &sequence_map = pair.second;
      // Iterate through sequences received from this endpoint
      for (auto it = sequence_map.begin(); it != sequence_map.end();) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second); // Time since received
        if (elapsed > SEQUENCE_RETAIN_TIME) {
          // Erase and get iterator to the next element
          it = sequence_map.erase(it);
        } else {
          // Only increment if not erased
          ++it;
        }
      }
    }
  }

  // Clean up recent NAKs map
  {
    std::lock_guard<std::mutex> lock(recent_naks_mutex_);
    for (auto it = recent_naks_.begin(); it != recent_naks_.end();
         /* no increment */) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - it->second); // Time since NAK sent
      // NAK debounce time is typically shorter than sequence retain time
      if (elapsed > NAK_DEBOUNCE_TIME *
                        2) { // Keep slightly longer than debounce to be safe
        it = recent_naks_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Clean up ACKed sequences map (relevant for Base Station)
  {
    std::lock_guard<std::mutex> lock(acked_sequences_mutex_);
    // Simple approach: Clear the entire ACKed map periodically.
    if (now > last_ack_cleanup_time_ + CLEANUP_INTERVAL * 2) {
      acked_sequences_.clear();
      last_ack_cleanup_time_ = now;
    }
  }

  // Note: sent_packets_ are implicitly cleaned up when ACKed (Rover) or max
  // retries reached (Rover). Base station keeps sent packets until NAKed or
  // potentially cleaned up if a very old packet gets NAKed (edge case).
}

// Periodic timer handler for cleaning up old tracking entries.
void ReliabilityManager::handle_cleanup_timer() {
  if (!running_) {
    return;
  }

  cleanup_old_entries();

  // Reschedule the timer.
  if (running_) {
    cleanup_timer_.expires_at(cleanup_timer_.expiry() + CLEANUP_INTERVAL);
    cleanup_timer_.async_wait([this](const boost::system::error_code &error) {
      if (!error && running_) {
        handle_cleanup_timer();
      }
    });
  }
}

// Records that the Base Station has sent an ACK for a specific sequence from an
// endpoint.
void ReliabilityManager::record_acked_sequence(uint8_t seq,
                                               const udp::endpoint &endpoint) {
  if (role_ != Role::BASE_STATION)
    return; // Only Base Station tracks sent ACKs

  std::string endpoint_key = get_endpoint_key(endpoint);
  std::lock_guard<std::mutex> lock(acked_sequences_mutex_);
  // Add the sequence number to the set of ACKed sequences for this endpoint.
  acked_sequences_[endpoint_key].insert(seq);
}

// Checks if the Base Station has previously sent an ACK for a specific sequence
// from an endpoint.
bool ReliabilityManager::has_acked_sequence(uint8_t seq,
                                            const udp::endpoint &endpoint) {
  if (role_ != Role::BASE_STATION)
    return false; // Only Base Station tracks sent ACKs

  std::string endpoint_key = get_endpoint_key(endpoint);
  std::lock_guard<std::mutex> lock(acked_sequences_mutex_);

  auto endpoint_it = acked_sequences_.find(endpoint_key);
  if (endpoint_it == acked_sequences_.end()) {
    return false; // No ACKs recorded for this endpoint yet.
  }

  // Check if the sequence number exists in the set for this endpoint.
  return endpoint_it->second.count(seq) > 0;
}

void ReliabilityManager::reset_state() {
  std::lock_guard<std::mutex> lock_sent(sent_packets_mutex_);
  sent_packets_.clear();
  std::lock_guard<std::mutex> lock_recv(received_sequences_mutex_);
  received_sequences_.clear();
  std::lock_guard<std::mutex> lock_nak(recent_naks_mutex_);
  recent_naks_.clear();
  std::lock_guard<std::mutex> lock_ack(acked_sequences_mutex_);
  acked_sequences_.clear();
  consecutive_timeouts_to_base_ = 0;
  std::cout << "[RELIABILITY] State reset." << std::endl;
}