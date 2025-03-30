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

const LumenHeader &LumenPacket::get_header() const { return header_; }

const std::vector<uint8_t> &LumenPacket::get_payload() const {
  return payload_;
}

std::vector<uint8_t> LumenPacket::to_bytes() const {
  // start with the header bytes
  std::vector<uint8_t> packet = header_.to_bytes();

  // add payload
  packet.insert(packet.end(), payload_.begin(), payload_.end());

  // add ETX
  packet.push_back(LumenHeader::ETX);

  return packet;
}

size_t LumenPacket::total_size() const {
  // header size + payload size + 1 for CRC + 1 fro ETX
  return LumenHeader::HEADER_SIZE + payload_.size() + 2;
}

bool LumenPacket::is_valid() const {
  std::vector<uint8_t> packet_data = to_bytes();
  if (packet_data.size() < LumenHeader::HEADER_SIZE + 2) {
    return false;
  }

  if (packet_data.back() != LumenHeader::ETX) {
    return false;
  }

  // verify the crc checksum
  size_t crc_pos = get_crc_position(packet_data);
  std::vector<uint8_t> data_for_crc(packet_data.begin(),
                                    packet_data.begin() + crc_pos);

  uint8_t calculated_crc = LumenHeader::calculate_crc8(data_for_crc);
  uint8_t stored_crc = packet_data[crc_pos];
  return calculated_crc == stored_crc;
}

size_t LumenPacket::get_crc_position(const std::vector<uint8_t> &packet_data) {
  // crc is located right before the etx byte
  return packet_data.size() - 2;
}

uint8_t LumenPacket::calculate_packet_crc() const {
  std::vector<uint8_t> data_for_crc;

  // add header bytes
  std::vector<uint8_t> header_bytes = header_.to_bytes();
  data_for_crc.insert(data_for_crc.end(), header_bytes.begin(),
                      header_bytes.end());

  // add payload bytes
  data_for_crc.insert(data_for_crc.end(), payload_.begin(), payload_.end());

  // calculate crc over combined data
  return LumenHeader::calculate_crc8(data_for_crc);
}