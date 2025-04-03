// include/common/lumen_header.hpp

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

// represents the fixed-size header structure for lumen protocol packets
class LumenHeader {
public:
  // defines the type of payload contained within the lumenpacket
  enum class MessageType : uint8_t {
    ACK = 0,
    CMD = 1,
    DATA = 2,
    VIDEO = 3,
    STATUS = 4,
    NAK = 5 // negative acknowledgement (retransmission request)
  };

  // defines the priority level of the message
  enum class Priority : uint8_t { LOW = 0, MEDIUM = 1, HIGH = 2 };

  LumenHeader(MessageType type, Priority prio, uint8_t seq, uint32_t timestamp,
              uint16_t payload_length);

  static std::optional<LumenHeader>
  from_bytes(const std::vector<uint8_t> &bytes);

  std::vector<uint8_t> to_bytes() const;

  // --- accessors ---
  MessageType get_type() const;
  Priority get_priority() const;
  uint8_t get_sequence() const;
  uint32_t get_timestamp() const;
  uint16_t get_payload_length() const;

  // --- mutators ---
  void set_sequence(uint8_t seq);
  void set_timestamp(uint32_t timestamp);
  void set_payload_length(uint16_t length);

  // calculates the crc8 checksum for a given byte vector
  static uint8_t calculate_crc8(const std::vector<uint8_t> &data);

private:
  MessageType type_;
  Priority priority_;
  uint8_t sequence_;
  uint32_t timestamp_;
  uint16_t payload_length_;
};