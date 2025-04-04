// include/common/lumen_packet.hpp

#pragma once

#include "lumen_header.hpp"
#include <cstdint>
#include <optional>
#include <vector>

// represents a complete lumen protocol data unit
class LumenPacket {
public:
  LumenPacket(const LumenHeader &header, const std::vector<uint8_t> &payload);

  static std::optional<LumenPacket>
  from_bytes(const std::vector<uint8_t> &bytes);

  std::vector<uint8_t> to_bytes() const;

  const LumenHeader &get_header() const;
  const std::vector<uint8_t> &get_payload() const;

  bool is_valid() const;

  size_t total_size() const;

  static size_t get_crc_position(const std::vector<uint8_t> &packet_data);

private:
  LumenHeader header_;
  std::vector<uint8_t> payload_;

  // calculates the crc8 checksum over the header and payload
  uint8_t calculate_packet_crc() const;
};