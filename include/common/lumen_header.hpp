// include/common/lumen_header.hpp

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "configs.hpp"

class LumenHeader {
public:
  enum class MessageType : uint8_t {
    ACK = 0,
    CMD = 1,
    DATA = 2,
    VIDEO = 3,
    STATUS = 4,
    SACK = 5
  };

  enum class Priority : uint8_t {
    LOW = 0,    // video
    MEDIUM = 1, // telemetry
    HIGH = 2    // commands
  };

  // constructor
  LumenHeader(MessageType type, Priority prio, uint8_t seq, uint32_t timestamp,
              uint16_t payload_length);

  static std::optional<LumenHeader>
  from_bytes(const std::vector<uint8_t> &bytes);
  std::vector<uint8_t> to_bytes() const;

  // accessors
  MessageType get_type() const;
  Priority get_priority() const;
  uint8_t get_sequence() const;
  uint32_t get_timestamp() const;
  uint16_t get_payload_length() const;

  // mutators
  void set_sequence(uint8_t seq);
  void set_timestamp(uint32_t timestamp);
  void set_payload_length(uint16_t length);

  static uint8_t calculate_crc8(const std::vector<uint8_t> &data);

private:
  MessageType type_;
  Priority priority_;
  uint8_t sequence_;
  uint32_t timestamp_;
  uint16_t payload_length_;
};