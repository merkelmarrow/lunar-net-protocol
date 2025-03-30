// src/common/lumen_packet.cpp

#include "lumen_packet.hpp"
#include "lumen_header.hpp"
#include <iostream>
#include <optional>
#include <vector>

LumenPacket::LumenPacket(const LumenHeader &header,
                         const std::vector<uint8_t> &payload)
    : header_(header), payload_(payload) {
  if (header_.get_payload_length() != payload_.size()) {
    header_.set_payload_length(payload.size());
  }
}

std::optional<LumenPacket>
LumenPacket::from_bytes(const std::vector<uint8_t> &bytes) {
  // try to get the header first
  auto header_opt = LumenHeader::from_bytes(bytes);

  if (!header_opt) {
    return std::nullopt;
  }

  LumenHeader header = *header_opt;

  // calculate the expected packet size
  size_t expected_size = LumenHeader::HEADER_SIZE +
                         header.get_payload_length() +
                         2; // +1 for ETX, +1 for CRC

  // validate packet size
  if (bytes.size() < expected_size) {
    return std::nullopt;
  }

  // check etx
  if (bytes[expected_size - 1] != LumenHeader::ETX) {
    return std::nullopt;
  }

  // extract payload
  std::vector<uint8_t> payload(bytes.begin() + LumenHeader::PAYLOAD_POS,
                               bytes.begin() + LumenHeader::PAYLOAD_POS +
                                   header.get_payload_length());

  // create packet
  LumenPacket packet(header, payload);

  // extract the stored crc
  uint8_t stored_crc = bytes[expected_size - 2];

  // calculate the expected crc
  std::vector<uint8_t> data_for_crc;
  data_for_crc.insert(data_for_crc.end(), bytes.begin(),
                      bytes.begin() + expected_size - 2);

  uint8_t calculated_crc = LumenHeader::calculate_crc8(data_for_crc);

  // verify crc
  if (calculated_crc != stored_crc) {
    std::cout << "[ERROR] CRC mismatch: calculated = "
              << static_cast<int>(calculated_crc)
              << ", stored = " << static_cast<int>(stored_crc) << std::endl;
    return std::nullopt;
  }

  return packet;
}