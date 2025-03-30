// include/common/lumen_header.hpp

#pragma once

#include <cstdint>

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
};