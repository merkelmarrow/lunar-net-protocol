// include/common/lumen_header.hpp

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

class LumenHeader {
public:
  // constants for state delims
  static constexpr uint8_t STX = 0x02;
  static constexpr uint8_t ETX = 0x03;

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
              uint8_t payload_length);

  static std::optional<LumenHeader>
  from_bytes(const std::vector<uint8_t> &bytes);

  static constexpr size_t STX_POS = 0;
  static constexpr size_t TYPE_POS = 1;
  static constexpr size_t PRIO_POS = 2;
  static constexpr size_t SEQ_POS = 3;
  static constexpr size_t TIMESTAMP_POS = 4;
  static constexpr size_t LEN_POS = 8;
  static constexpr size_t PAYLOAD_POS = 9;
  static constexpr size_t HEADER_SIZE = 9;

  static uint8_t calculate_crc8(const std::vector<uint8_t> &data);

private:
  MessageType type_;
  Priority priority_;
  uint8_t sequence_;
  uint32_t timestamp_;
  uint8_t payload_length_;
};