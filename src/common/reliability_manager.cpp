// src/common/reliability_manager.cpp
#include "reliability_manager.hpp"
#include "configs.hpp"
#include <iostream>
#include <sstream>

using boost::asio::ip::udp;

// helper function to create a consistent string key from an endpoint
std::string get_endpoint_key(const udp::endpoint &endpoint) {
  std::stringstream ss;
  ss << endpoint.address().to_string() << ":" << endpoint.port();
  return ss.str();
}

ReliabilityManager::ReliabilityManager(boost::asio::io_context &io_context,
                                       bool is_base_station)
    : retransmit_timer_(io_context, RELIABILITY_CHECK_INTERVAL),
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

  // schedule the first checks
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

  // store packet details for potential retransmission or nak handling
  sent_packets_.emplace(
      seq,
      SentPacketInfo{packet, std::chrono::steady_clock::now(), 0, recipient});
}

void ReliabilityManager::process_ack(uint8_t seq) {
  // acks are only processed by the rover
  if (!running_ || role_ != Role::ROVER) {
    return;
  }

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  auto it = sent_packets_.find(seq);
  if (it != sent_packets_.end()) {
    // packet acknowledged, remove it from tracking
    consecutive_timeouts_to_base_ = 0;
    sent_packets_.erase(it);
  } else {
    // benign if ack arrives after retransmission or is duplicate
  }
}

void ReliabilityManager::process_nak(uint8_t seq) {
  // naks are only processed by the base station
  if (!running_ || role_ != Role::BASE_STATION) {
    return;
  }

  // note: assumes immediate retransmission upon nak

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  auto it = sent_packets_.find(seq);
  if (it != sent_packets_.end()) {
    // packet requested via nak is found, retransmit it
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
      // base station retransmits only when receiving a nak
      callback_copy(it->second.packet, it->second.recipient);
    } else {
      std::cerr << "[RELIABILITY] Error: NAK received for seq "
                << static_cast<int>(seq)
                << " but retransmit callback is not set!" << std::endl;
    }
  } else {
    // requested sequence might be old or never sent
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

  // store the time when a sequence number was received from a specific endpoint
  received_sequences_[endpoint_key][seq] = std::chrono::steady_clock::now();
}

std::vector<uint8_t>
ReliabilityManager::get_missing_sequences(const udp::endpoint &sender) {
  // only the rover needs to detect missing sequences to send naks
  if (role_ != Role::ROVER) {
    return {};
  }

  std::string endpoint_key = get_endpoint_key(sender);
  std::vector<uint8_t> missing_seqs;

  std::lock_guard<std::mutex> lock(received_sequences_mutex_);

  auto endpoint_it = received_sequences_.find(endpoint_key);
  if (endpoint_it == received_sequences_.end() || endpoint_it->second.empty()) {
    return missing_seqs; // no data received yet
  }

  auto &sequence_map = endpoint_it->second;

  // find the highest sequence number received from this sender so far
  uint8_t highest_seq = sequence_map.rbegin()->first;

  // check backwards from the highest received sequence number within a defined
  // window
  for (uint8_t i = 1; i <= WINDOW_SIZE; ++i) {
    uint8_t seq_to_check = highest_seq - i;

    if (sequence_map.find(seq_to_check) == sequence_map.end()) {
      // might be missing, add to list
      missing_seqs.push_back(seq_to_check);
    }
  }
  // lumenprotocol uses this list and checks is_recently_naked before sending
  // naks
  return missing_seqs;
}

bool ReliabilityManager::is_recently_naked(uint8_t seq) {
  std::lock_guard<std::mutex> lock(recent_naks_mutex_);

  auto it = recent_naks_.find(seq);
  if (it == recent_naks_.end()) {
    return false; // haven't sent a nak recently
  }

  // check if recorded nak time is within debounce interval
  auto now = std::chrono::steady_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);

  return elapsed < NAK_DEBOUNCE_TIME;
}

void ReliabilityManager::record_nak_sent(uint8_t seq) {
  // record the time when a nak for this sequence was sent
  std::lock_guard<std::mutex> lock(recent_naks_mutex_);
  recent_naks_[seq] = std::chrono::steady_clock::now();
}

// this function is primarily used by the rover's retransmission timer
std::vector<std::pair<LumenPacket, udp::endpoint>>
ReliabilityManager::get_packets_to_retransmit() {

  std::vector<std::pair<LumenPacket, udp::endpoint>> packets_to_retransmit;
  auto now = std::chrono::steady_clock::now();

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  for (auto it = sent_packets_.begin(); it != sent_packets_.end();) {
    auto &info = it->second;
    uint8_t seq = it->first;

    // --- role-specific retransmission logic ---

    // base station: never retransmits based on timer, only on nak
    if (role_ == Role::BASE_STATION) {
      ++it;
      continue;
    }

    // rover: uses timeout-based retransmission
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - info.sent_time);
    // calculate exponential backoff timeout
    auto timeout = RELIABILITY_BASE_TIMEOUT *
                   (1 << info.retry_count); // timeout = base * 2^retry_count

    if (elapsed > timeout) {
      // timeout exceeded, check max retries
      if (info.retry_count >= RELIABILITY_MAX_RETRIES) {
        bool timeout_to_base = false;
        // todo: replace port check with more robust base station identification
        if (info.recipient.port() == 9000) {
          timeout_to_base = true;
        }

        if (timeout_to_base) {
          std::cerr << "[RELIABILITY] Max retries (" << RELIABILITY_MAX_RETRIES
                    << ") reached for packet seq: " << static_cast<int>(seq)
                    << " to Base Station. Notifying Rover to disconnect."
                    << std::endl;

          // notify rover about the timeout via callback
          TimeoutCallback callback_copy;
          {
            std::lock_guard<std::mutex> cb_lock(timeout_callback_mutex_);
            callback_copy = timeout_callback_;
          }

          if (callback_copy) {
            try {
              callback_copy(info.recipient);
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
          // max retries reached for a non-base recipient
          std::cerr << "[RELIABILITY] Max retries (" << RELIABILITY_MAX_RETRIES
                    << ") reached for packet seq: " << static_cast<int>(seq)
                    << " to non-base recipient " << info.recipient
                    << ". Giving up." << std::endl;
        }
        // erase the packet from tracking
        it = sent_packets_.erase(it);
      } else {
        // add packet to the list for retransmission
        packets_to_retransmit.push_back({info.packet, info.recipient});

        // update packet info: reset sent_time and increment retry_count
        info.sent_time = now;
        info.retry_count++;

        std::cout << "[RELIABILITY] Timeout detected for packet seq: "
                  << static_cast<int>(seq)
                  << ". Queued for retransmission (attempt " << info.retry_count
                  << ")." << std::endl;
        ++it;
      }
    } else {
      // packet has not timed out yet
      ++it;
    }
  } // end of loop

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

// periodic timer handler, primarily for rover's timeout-based retransmissions
void ReliabilityManager::handle_retransmission_timer() {
  if (!running_) {
    return;
  }

  // this check is only relevant for the rover
  if (role_ == Role::ROVER) {
    auto packets = get_packets_to_retransmit();

    if (!packets.empty()) {
      std::function<void(const LumenPacket &, const udp::endpoint &)>
          callback_copy;
      {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback_copy = retransmit_callback_;
      }

      // trigger retransmission via the callback (set by lumenprotocol)
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
  // base station doesn't use this timer for retransmissions

  // reschedule the timer for the next check
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

// removes old entries from tracking maps to prevent infinite growth
void ReliabilityManager::cleanup_old_entries() {
  auto now = std::chrono::steady_clock::now();

  // clean up received sequences map
  {
    std::lock_guard<std::mutex> lock(received_sequences_mutex_);
    for (auto &pair : received_sequences_) {
      auto &sequence_map = pair.second;
      for (auto it = sequence_map.begin(); it != sequence_map.end();) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second);
        if (elapsed > SEQUENCE_RETAIN_TIME) {
          it = sequence_map.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  // clean up recent naks map
  {
    std::lock_guard<std::mutex> lock(recent_naks_mutex_);
    for (auto it = recent_naks_.begin(); it != recent_naks_.end();) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - it->second);
      if (elapsed >
          NAK_DEBOUNCE_TIME * 2) { // keep slightly longer than debounce
        it = recent_naks_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // clean up acked sequences map (relevant for base station)
  {
    std::lock_guard<std::mutex> lock(acked_sequences_mutex_);
    // simple approach: clear the entire acked map periodically
    if (now > last_ack_cleanup_time_ + CLEANUP_INTERVAL * 2) {
      acked_sequences_.clear();
      last_ack_cleanup_time_ = now;
    }
  }

  // sent_packets_ are cleaned up when acked or max retries reached
}

// periodic timer handler for cleaning up old tracking entries
void ReliabilityManager::handle_cleanup_timer() {
  if (!running_) {
    return;
  }

  cleanup_old_entries();

  // reschedule the timer
  if (running_) {
    cleanup_timer_.expires_at(cleanup_timer_.expiry() + CLEANUP_INTERVAL);
    cleanup_timer_.async_wait([this](const boost::system::error_code &error) {
      if (!error && running_) {
        handle_cleanup_timer();
      }
    });
  }
}

// records that the base station has sent an ack for a specific sequence
void ReliabilityManager::record_acked_sequence(uint8_t seq,
                                               const udp::endpoint &endpoint) {
  if (role_ != Role::BASE_STATION)
    return;

  std::string endpoint_key = get_endpoint_key(endpoint);
  std::lock_guard<std::mutex> lock(acked_sequences_mutex_);
  acked_sequences_[endpoint_key].insert(seq);
}

// checks if the base station has previously sent an ack for a specific sequence
bool ReliabilityManager::has_acked_sequence(uint8_t seq,
                                            const udp::endpoint &endpoint) {
  if (role_ != Role::BASE_STATION)
    return false;

  std::string endpoint_key = get_endpoint_key(endpoint);
  std::lock_guard<std::mutex> lock(acked_sequences_mutex_);

  auto endpoint_it = acked_sequences_.find(endpoint_key);
  if (endpoint_it == acked_sequences_.end()) {
    return false; // no acks recorded for this endpoint yet
  }

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