// src/common/lumen_header.cpp

#include "lumen_header.hpp"
#include "configs.hpp"
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

LumenHeader::LumenHeader(MessageType type, Priority prio, uint8_t seq,
                         uint32_t timestamp, uint16_t payload_length)
    : type_(type), priority_(prio), sequence_(seq), timestamp_(timestamp),
      payload_length_(payload_length) {}

// factory method: attempts to parse a lumenheader from byte vector beginning
std::optional<LumenHeader>
LumenHeader::from_bytes(const std::vector<uint8_t> &bytes) {
  // check minimum size
  if (bytes.size() < LUMEN_HEADER_SIZE) {
    return std::nullopt;
  }

  // check stx marker
  if (bytes[LUMEN_STX_POS] != LUMEN_STX) {
    std::cerr << "[ERROR] LumenHeader::from_bytes: Missing or incorrect STX "
                 "marker. Found: 0x"
              << std::hex << static_cast<int>(bytes[LUMEN_STX_POS]) << std::dec
              << std::endl;
    return std::nullopt;
  }

  // extract fields based on defined positions
  MessageType type = static_cast<MessageType>(bytes[LUMEN_TYPE_POS]);
  Priority priority = static_cast<Priority>(bytes[LUMEN_PRIO_POS]);
  uint8_t sequence = bytes[LUMEN_SEQ_POS];

  // extract 4-byte timestamp (big-endian)
  uint32_t timestamp = 0;
  timestamp |= static_cast<uint32_t>(bytes[LUMEN_TIMESTAMP_POS]) << 24;
  timestamp |= static_cast<uint32_t>(bytes[LUMEN_TIMESTAMP_POS + 1]) << 16;
  timestamp |= static_cast<uint32_t>(bytes[LUMEN_TIMESTAMP_POS + 2]) << 8;
  timestamp |= static_cast<uint32_t>(bytes[LUMEN_TIMESTAMP_POS + 3]);

  // extract 2-byte payload length (big-endian)
  uint16_t payload_length = 0;
  payload_length |= static_cast<uint16_t>(bytes[LUMEN_LEN_POS]) << 8;
  payload_length |= static_cast<uint16_t>(bytes[LUMEN_LEN_POS + 1]);

  // basic validation for enum ranges
  // assumes max enum value for messagetype is 5 (nak)
  if (static_cast<uint8_t>(type) > 5) {
    std::cerr << "[ERROR] LumenHeader::from_bytes: Invalid MessageType value: "
              << static_cast<int>(type) << std::endl;
    return std::nullopt;
  }
  // assumes max enum value for priority is 2 (high)
  if (static_cast<uint8_t>(priority) > 2) {
    std::cerr << "[ERROR] LumenHeader::from_bytes: Invalid Priority value: "
              << static_cast<int>(priority) << std::endl;
    return std::nullopt;
  }

  // all checks passed
  return LumenHeader(type, priority, sequence, timestamp, payload_length);
}

// serializes the header object into a fixed-size byte vector
std::vector<uint8_t> LumenHeader::to_bytes() const {
  std::vector<uint8_t> bytes(LUMEN_HEADER_SIZE);

  bytes[LUMEN_STX_POS] = LUMEN_STX;
  bytes[LUMEN_TYPE_POS] = static_cast<uint8_t>(type_);
  bytes[LUMEN_PRIO_POS] = static_cast<uint8_t>(priority_);
  bytes[LUMEN_SEQ_POS] = sequence_;

  // serialize timestamp (4 bytes, big-endian)
  bytes[LUMEN_TIMESTAMP_POS] = (timestamp_ >> 24) & 0xFF;
  bytes[LUMEN_TIMESTAMP_POS + 1] = (timestamp_ >> 16) & 0xFF;
  bytes[LUMEN_TIMESTAMP_POS + 2] = (timestamp_ >> 8) & 0xFF;
  bytes[LUMEN_TIMESTAMP_POS + 3] = timestamp_ & 0xFF;

  // serialize payload length (2 bytes, big-endian)
  bytes[LUMEN_LEN_POS] = (payload_length_ >> 8) & 0xFF;
  bytes[LUMEN_LEN_POS + 1] = payload_length_ & 0xFF;

  return bytes;
}

// static method to calculate crc8 checksum (used by lumenpacket)
// polynomial: 0x07 (x^8 + x^2 + x + 1)
uint8_t LumenHeader::calculate_crc8(const std::vector<uint8_t> &data) {
  uint8_t crc = 0x00; // initial value

  for (uint8_t byte : data) {
    crc ^= byte;                  // xor byte into crc
    for (int i = 0; i < 8; ++i) { // process each bit
      if (crc & 0x80) {           // if msb is 1
        crc = (crc << 1) ^ 0x07;  // shift left and xor with polynomial
      } else {
        crc <<= 1; // shift left
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
uint16_t LumenHeader::get_payload_length() const { return payload_length_; }

// mutators
void LumenHeader::set_sequence(uint8_t seq) { sequence_ = seq; }
void LumenHeader::set_timestamp(uint32_t timestamp) { timestamp_ = timestamp; }
void LumenHeader::set_payload_length(uint16_t length) {
  payload_length_ = length;
}