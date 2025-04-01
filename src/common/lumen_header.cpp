// src/common/lumen_header.cpp

#include "lumen_header.hpp"
#include "configs.hpp" // For header layout constants (LUMEN_STX_POS, etc.) and LUMEN_STX
#include <cstdint>
#include <iostream> // Include iostream for error logging
#include <optional>
#include <vector> // Include vector for method signatures

LumenHeader::LumenHeader(MessageType type, Priority prio, uint8_t seq,
                         uint32_t timestamp, uint16_t payload_length)
    : type_(type), priority_(prio), sequence_(seq), timestamp_(timestamp),
      payload_length_(payload_length) {}

// Factory method: Attempts to parse a LumenHeader from the beginning of a byte
// vector.
std::optional<LumenHeader>
LumenHeader::from_bytes(const std::vector<uint8_t> &bytes) {
  // Check if there are enough bytes for the fixed-size header.
  if (bytes.size() < LUMEN_HEADER_SIZE) {
    // std::cerr << "[DEBUG] LumenHeader::from_bytes: Insufficient bytes (" <<
    // bytes.size() << " < " << LUMEN_HEADER_SIZE << ")" << std::endl;
    return std::nullopt;
  }

  // Check for the Start Transmission (STX) marker at the beginning.
  if (bytes[LUMEN_STX_POS] != LUMEN_STX) {
    std::cerr << "[ERROR] LumenHeader::from_bytes: Missing or incorrect STX "
                 "marker. Found: 0x"
              << std::hex << static_cast<int>(bytes[LUMEN_STX_POS]) << std::dec
              << std::endl;
    return std::nullopt;
  }

  // Extract fields according to the defined positions (from configs.hpp).
  MessageType type = static_cast<MessageType>(bytes[LUMEN_TYPE_POS]);
  Priority priority = static_cast<Priority>(bytes[LUMEN_PRIO_POS]);
  uint8_t sequence = bytes[LUMEN_SEQ_POS];

  // Extract the 4-byte timestamp (Big-Endian).
  uint32_t timestamp = 0;
  timestamp |= static_cast<uint32_t>(bytes[LUMEN_TIMESTAMP_POS]) << 24;
  timestamp |= static_cast<uint32_t>(bytes[LUMEN_TIMESTAMP_POS + 1]) << 16;
  timestamp |= static_cast<uint32_t>(bytes[LUMEN_TIMESTAMP_POS + 2]) << 8;
  timestamp |= static_cast<uint32_t>(bytes[LUMEN_TIMESTAMP_POS + 3]);

  // Extract the 2-byte payload length (Big-Endian).
  uint16_t payload_length = 0;
  payload_length |= static_cast<uint16_t>(bytes[LUMEN_LEN_POS]) << 8;
  payload_length |= static_cast<uint16_t>(bytes[LUMEN_LEN_POS + 1]);

  // Basic validation for enum ranges (adjust if enums change).
  if (static_cast<uint8_t>(type) >
      5) { // Assuming max enum value for MessageType is 5 (NAK)
    std::cerr << "[ERROR] LumenHeader::from_bytes: Invalid MessageType value: "
              << static_cast<int>(type) << std::endl;
    return std::nullopt;
  }
  if (static_cast<uint8_t>(priority) >
      2) { // Assuming max enum value for Priority is 2 (HIGH)
    std::cerr << "[ERROR] LumenHeader::from_bytes: Invalid Priority value: "
              << static_cast<int>(priority) << std::endl;
    return std::nullopt;
  }

  // All checks passed, create and return the header object.
  return LumenHeader(type, priority, sequence, timestamp, payload_length);
}

// Serializes the header object into a fixed-size byte vector.
std::vector<uint8_t> LumenHeader::to_bytes() const {
  std::vector<uint8_t> bytes(LUMEN_HEADER_SIZE);

  bytes[LUMEN_STX_POS] = LUMEN_STX;
  bytes[LUMEN_TYPE_POS] = static_cast<uint8_t>(type_);
  bytes[LUMEN_PRIO_POS] = static_cast<uint8_t>(priority_);
  bytes[LUMEN_SEQ_POS] = sequence_;

  // Serialize timestamp (4 bytes, Big-Endian).
  bytes[LUMEN_TIMESTAMP_POS] = (timestamp_ >> 24) & 0xFF;
  bytes[LUMEN_TIMESTAMP_POS + 1] = (timestamp_ >> 16) & 0xFF;
  bytes[LUMEN_TIMESTAMP_POS + 2] = (timestamp_ >> 8) & 0xFF;
  bytes[LUMEN_TIMESTAMP_POS + 3] = timestamp_ & 0xFF;

  // Serialize payload length (2 bytes, Big-Endian).
  bytes[LUMEN_LEN_POS] = (payload_length_ >> 8) & 0xFF;
  bytes[LUMEN_LEN_POS + 1] = payload_length_ & 0xFF;

  return bytes;
}

// Static method to calculate CRC8 checksum (used by LumenPacket).
// Polynomial: 0x07 (x^8 + x^2 + x + 1)
uint8_t LumenHeader::calculate_crc8(const std::vector<uint8_t> &data) {
  uint8_t crc = 0x00; // Initial value

  for (uint8_t byte : data) {
    crc ^= byte;                  // XOR byte into CRC
    for (int i = 0; i < 8; ++i) { // Process each bit
      if (crc & 0x80) {           // If MSB is 1
        crc = (crc << 1) ^ 0x07;  // Shift left and XOR with polynomial
      } else {
        crc <<= 1; // Shift left
      }
    }
  }
  return crc;
}

// Accessors
LumenHeader::MessageType LumenHeader::get_type() const { return type_; }
LumenHeader::Priority LumenHeader::get_priority() const { return priority_; }
uint8_t LumenHeader::get_sequence() const { return sequence_; }
uint32_t LumenHeader::get_timestamp() const { return timestamp_; }
uint16_t LumenHeader::get_payload_length() const { return payload_length_; }

// Mutators
void LumenHeader::set_sequence(uint8_t seq) { sequence_ = seq; }
void LumenHeader::set_timestamp(uint32_t timestamp) { timestamp_ = timestamp; }
void LumenHeader::set_payload_length(uint16_t length) {
  payload_length_ = length;
} // Renamed parameter for clarity