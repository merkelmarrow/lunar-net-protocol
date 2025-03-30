#include "lumen_header.hpp"
#include <cstdint>
#include <optional>

#include "configs.hpp"

LumenHeader::LumenHeader(MessageType type, Priority prio, uint8_t seq,
                         uint32_t timestamp, uint8_t payload_length)
    : type_(type), priority_(prio), sequence_(seq), timestamp_(timestamp),
      payload_length_(payload_length) {}

// deserialise from raw bytes
std::optional<LumenHeader>
LumenHeader::from_bytes(const std::vector<uint8_t> &bytes) {
  // check if we have enough bytes for a header
  if (bytes.size() < LUMEN_HEADER_SIZE) {
    return std::nullopt;
  }

  // check stx
  if (bytes[LUMEN_STX_POS] != LUMEN_STX) {
    return std::nullopt;
  }

  // extract fields
  MessageType type = static_cast<MessageType>(bytes[LUMEN_TYPE_POS]);
  Priority priority = static_cast<Priority>(bytes[LUMEN_PRIO_POS]);
  uint8_t sequence = bytes[LUMEN_SEQ_POS];

  // exract the timestamp
  uint32_t timestamp = 0;
  timestamp |= static_cast<uint32_t>(bytes[LUMEN_TIMESTAMP_POS]) << 24;
  timestamp |= static_cast<uint32_t>(bytes[LUMEN_TIMESTAMP_POS + 1]) << 16;
  timestamp |= static_cast<uint32_t>(bytes[LUMEN_TIMESTAMP_POS + 2]) << 8;
  timestamp |= static_cast<uint32_t>(bytes[LUMEN_TIMESTAMP_POS + 3]);

  // get payload length
  uint8_t payload_length = bytes[LUMEN_LEN_POS];

  // validate message type and priority
  if (static_cast<uint8_t>(type) > 5 || static_cast<uint8_t>(priority) > 2) {
    return std::nullopt;
  }

  // create and return the header
  return LumenHeader(type, priority, sequence, timestamp, payload_length);
}

// serialise to bytes
std::vector<uint8_t> LumenHeader::to_bytes() const {
  std::vector<uint8_t> bytes(LUMEN_HEADER_SIZE);

  // start with transmission byte
  bytes[LUMEN_STX_POS] = LUMEN_STX;

  // message type
  bytes[LUMEN_TYPE_POS] = static_cast<uint8_t>(type_);

  // priority
  bytes[LUMEN_PRIO_POS] = static_cast<uint8_t>(priority_);

  // sequence number
  bytes[LUMEN_SEQ_POS] = sequence_;

  // timestamp (4 bytes, big-endian)
  bytes[LUMEN_TIMESTAMP_POS] = (timestamp_ >> 24) & 0xFF;
  bytes[LUMEN_TIMESTAMP_POS + 1] = (timestamp_ >> 16) & 0xFF;
  bytes[LUMEN_TIMESTAMP_POS + 2] = (timestamp_ >> 8) & 0xFF;
  bytes[LUMEN_TIMESTAMP_POS + 3] = timestamp_ & 0xFF;

  // payload length
  bytes[LUMEN_LEN_POS] = payload_length_;

  return bytes;
}

// CRC-8 calculation
uint8_t LumenHeader::calculate_crc8(const std::vector<uint8_t> &data) {
  uint8_t crc = 0;

  for (uint8_t byte : data) {
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
      if (crc & 0x80) {
        crc = (crc << 1) ^ 0x07; // polynomial x^8 + x^2 + x + 1
      } else {
        crc <<= 1;
      }
    }
  }

  return crc;
}

// accessors
LumenHeader::MessageType LumenHeader::get_type() const { return type_; }

LumenHeader::Priority LumenHeader::get_priority() const { return priority_; }

uint8_t LumenHeader::get_sequence() const { return sequence_; }

uint32_t LumenHeader::get_timestamp() const { return timestamp_; }

uint8_t LumenHeader::get_payload_length() const { return payload_length_; }

// mutators
void LumenHeader::set_sequence(uint8_t seq) { sequence_ = seq; }

void LumenHeader::set_timestamp(uint32_t timestamp) { timestamp_ = timestamp; }

void LumenHeader::set_payload_length(uint8_t length) {
  payload_length_ = length;
}