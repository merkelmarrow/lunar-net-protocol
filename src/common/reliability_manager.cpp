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
  if (!running_ || missing_seqs.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(sent_packets_mutex_);

  std::cout << "[RELIABILITY] Received SACK with " << missing_seqs.size()
            << " missing sequences." << std::endl;

  // the first sequence in the SACK is the next expected sequence
  uint8_t next_expected = missing_seqs[0];

  // create a set of missing sequences for faster lookup
  std::set<uint8_t> missing_set(missing_seqs.begin(), missing_seqs.end());

  // find the highest missing sequence to determine window size
  uint8_t highest_missing = next_expected;
  for (uint8_t missing_seq : missing_seqs) {
    bool is_higher = false;
    if (missing_seq > highest_missing && missing_seq - highest_missing < 128) {
      is_higher = true;
    } else if (missing_seq < highest_missing &&
               highest_missing - missing_seq > 128) {
      is_higher = true;
    }

    if (is_higher) {
      highest_missing = missing_seq;
    }
  }

  // process our sent packets
  auto it = sent_packets_.begin();
  while (it != sent_packets_.end()) {
    uint8_t seq = it->first;

    // Case 1: Is this sequence already acknowledged?
    // (i.e., is it before the next expected sequence?)
    bool is_acknowledged = false;

    if (seq == next_expected - 1) {
      // special case: this is the sequence right before next_expected
      is_acknowledged = true;
    } else if (next_expected > seq) {
      // normal case: next_expected is higher than seq
      // if the difference is small, this packet is acknowledged
      if (next_expected - seq < 128) {
        is_acknowledged = true;
      }
    } else if (next_expected < seq) {
      // wrap-around case: next_expected wrapped back to 0
      // if seq is close to 255, it's acknowledged
      if (seq - next_expected > 128) {
        is_acknowledged = true;
      }
    }

    if (is_acknowledged) {
      std::cout << "[RELIABILITY] SACK implicitly acknowledged seq: "
                << static_cast<int>(seq) << " (already acknowledged)"
                << std::endl;
      it = sent_packets_.erase(it);
      continue;
    }

    // case 2: Is this sequence explicitly missing?
    if (missing_set.find(seq) != missing_set.end()) {
      // keep it for retransmission
      ++it;
      continue;
    }

    // case 3: Is this sequence within the SACK window but not missing?

    // check if seq is within the window
    bool is_in_window = false;
    if (next_expected <= highest_missing) {
      // no wrap-around in window
      is_in_window = (seq >= next_expected && seq <= highest_missing);
    } else {
      // window wraps around
      is_in_window = (seq >= next_expected || seq <= highest_missing);
    }

    if (is_in_window) {
      std::cout << "[RELIABILITY] SACK explicitly acknowledged seq: "
                << static_cast<int>(seq) << " (not in missing set)"
                << std::endl;
      it = sent_packets_.erase(it);
    } else {
      ++it;
    }
  }

  // retransmit missing sequences
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
        // reset retransmission timer to avoid immediate retries
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

LumenPacket ReliabilityManager::generate_sack_packet() {
  std::lock_guard<std::mutex> lock(received_sequences_mutex_);

  uint8_t next_seq = next_expected_sequence_;
  std::vector<uint8_t> missing_seqs;

  // add next expected sequence
  missing_seqs.push_back(next_seq);

  // find the highest sequence we've actually received
  uint8_t highest_received = next_seq;
  for (uint8_t seq : received_sequences_) {
    // handle wrap-around when determining highest
    bool is_higher = false;
    if (seq > highest_received && seq - highest_received < 128) {
      is_higher = true;
    } else if (seq < highest_received && highest_received - seq > 128) {
      is_higher = true;
    }

    if (is_higher) {
      highest_received = seq;
    }
  }

  // calculate how many sequences to look for gaps
  uint8_t range;
  if (highest_received >= next_seq) {
    range = highest_received - next_seq;
  } else {
    range = (highest_received + 256) - next_seq;
  }

  // cap the range to avoid excessive SACK sizes
  range = std::min(range, (uint8_t)16);

  // look for actual gaps in the received sequences
  for (uint8_t i = 1; i <= range; i++) {
    uint8_t seq = (next_seq + i) & 0xFF;
    if (received_sequences_.find(seq) == received_sequences_.end()) {
      missing_seqs.push_back(seq);
    }
  }

  // create a SACK header
  LumenHeader header(LumenHeader::MessageType::SACK,
                     LumenHeader::Priority::HIGH, next_seq,
                     0, // Use timestamp 0 for SACK packets
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