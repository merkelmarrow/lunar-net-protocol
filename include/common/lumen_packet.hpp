// include/common/lumen_packet.hpp

#pragma once

#include "lumen_header.hpp"
#include <cstdint>
#include <optional>
#include <vector>

class LumenPacket {
public:
  // create a new packet
  LumenPacket(const LumenHeader &header, const std::vector<uint8_t> &payload);

  // parse packet from raw bytes
  static std::optional<LumenPacket>
  from_bytes(const std::vector<uint8_t> &bytes);

  std::vector<uint8_t> to_bytes() const;

  bool is_valid() const;

  size_t total_size() const;

  static size_t get_crc_position(const std::vector<uint8_t> &packet_data);

private:
  LumenHeader header_;
  std::vector<uint8_t> payload_;

  uint8_t calculate_packet_crc() const;
};