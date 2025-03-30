#include "lumen_header.hpp"
#include <cstdint>
#include <optional>

LumenHeader::LumenHeader(MessageType type, Priority prio, uint8_t seq,
                         uint32_t timestamp, uint8_t payload_length)
    : type_(type), priority_(prio), sequence_(seq), timestamp_(timestamp),
      payload_length_(payload_length) {}

// deserialise from raw bytes
std::optional<LumenHeader>
LumenHeader::from_bytes(const std::vector<uint8_t> &bytes) {
  // check if we have enough bytes for a header
  if (bytes.size() < HEADER_SIZE) {
    return std::nullopt;
  }

  // check stx
  if (bytes[STX_POS] != STX) {
    return std::nullopt;
  }

  // extract fields
  MessageType type = static_cast<MessageType>(bytes[TYPE_POS]);
  Priority priority = static_cast<Priority>(bytes[PRIO_POS]);
  uint8_t sequence = bytes[SEQ_POS];

  // exract the timestamp
  uint32_t timestamp = 0;
  timestamp |= static_cast<uint32_t>(bytes[TIMESTAMP_POS]) << 24;
  timestamp |= static_cast<uint32_t>(bytes[TIMESTAMP_POS + 1]) << 16;
  timestamp |= static_cast<uint32_t>(bytes[TIMESTAMP_POS + 2]) << 8;
  timestamp |= static_cast<uint32_t>(bytes[TIMESTAMP_POS + 3]);

  // get payload length
  uint8_t payload_length = bytes[LEN_POS];

  // validate message type and priority
  if (static_cast<uint8_t>(type) > 5 || static_cast<uint8_t>(priority) > 2) {
    return std::nullopt;
  }

  // create and return the header
  return LumenHeader(type, priority, sequence, timestamp, payload_length);
}