// src/common/lumen_packet.cpp

#include "lumen_packet.hpp"
#include "configs.hpp"
#include "lumen_header.hpp"
#include <iostream>
#include <optional>
#include <vector>

// constructor: takes header and payload, ensures payload length correct in
// header
LumenPacket::LumenPacket(const LumenHeader &header,
                         const std::vector<uint8_t> &payload)
    : header_(header), payload_(payload) {
  // ensure header reflects actual payload size
  header_.set_payload_length(static_cast<uint16_t>(payload_.size()));
}

// factory method: attempts to parse a lumenpacket from byte stream
std::optional<LumenPacket>
LumenPacket::from_bytes(const std::vector<uint8_t> &bytes) {
  // step 1: try parsing header first
  auto header_opt = LumenHeader::from_bytes(bytes);
  if (!header_opt) {
    // not enough bytes or stx missing/incorrect
    return std::nullopt;
  }
  LumenHeader header = *header_opt;

  // step 2: calculate expected total size based on header's payload length
  size_t expected_size = LUMEN_HEADER_SIZE + header.get_payload_length() +
                         2; // header + payload + crc + etx

  // step 3: check if enough bytes in buffer for full declared packet
  if (bytes.size() < expected_size) {
    // not enough data received yet
    return std::nullopt;
  }

  // step 4: check for etx marker at expected position
  if (bytes[expected_size - 1] != LUMEN_ETX) {
    std::cerr << "[ERROR] LumenPacket::from_bytes: Missing or incorrect ETX "
                 "marker. Expected size: "
              << expected_size << std::endl;
    return std::nullopt;
  }

  // step 5: extract payload data
  std::vector<uint8_t> payload(bytes.begin() + LUMEN_PAYLOAD_POS,
                               bytes.begin() + LUMEN_PAYLOAD_POS +
                                   header.get_payload_length());

  // step 6: verify crc checksum
  // extract stored crc byte (byte before etx)
  uint8_t stored_crc = bytes[expected_size - 2];

  // calculate crc based on header and payload bytes (all bytes before crc byte)
  std::vector<uint8_t> data_for_crc(bytes.begin(),
                                    bytes.begin() + expected_size - 2);
  uint8_t calculated_crc = LumenHeader::calculate_crc8(data_for_crc);

  if (calculated_crc != stored_crc) {
    std::cerr << "[ERROR] LumenPacket::from_bytes: CRC mismatch! Calculated=0x"
              << std::hex << static_cast<int>(calculated_crc) << ", Stored=0x"
              << static_cast<int>(stored_crc) << std::dec
              << ". Packet seq: " << static_cast<int>(header.get_sequence())
              << std::endl;
    // crc failed, packet corrupt
    return std::nullopt;
  }

  // step 7: all checks passed, create and return object
  return LumenPacket(header, payload);
}

const LumenHeader &LumenPacket::get_header() const { return header_; }

const std::vector<uint8_t> &LumenPacket::get_payload() const {
  return payload_;
}

// serializes the lumenpacket into a byte vector for transmission
std::vector<uint8_t> LumenPacket::to_bytes() const {
  // start with serialized header bytes
  std::vector<uint8_t> packet_bytes = header_.to_bytes();

  // append payload bytes
  packet_bytes.insert(packet_bytes.end(), payload_.begin(), payload_.end());

  // calculate crc over combined header and payload
  uint8_t crc = calculate_packet_crc(); // use internal helper
  packet_bytes.push_back(crc);

  // append etx marker
  packet_bytes.push_back(LUMEN_ETX);

  return packet_bytes;
}

// returns total expected size of packet on the wire
size_t LumenPacket::total_size() const {
  // header size + payload size + crc (1 byte) + etx (1 byte)
  return LUMEN_HEADER_SIZE + payload_.size() + 2;
}

// calculates crc8 checksum over packet's header and payload
uint8_t LumenPacket::calculate_packet_crc() const {
  std::vector<uint8_t> data_for_crc;
  data_for_crc.reserve(LUMEN_HEADER_SIZE + payload_.size()); // reserve space

  // get header bytes
  std::vector<uint8_t> header_bytes = header_.to_bytes();
  data_for_crc.insert(data_for_crc.end(), header_bytes.begin(),
                      header_bytes.end());

  // append payload bytes
  data_for_crc.insert(data_for_crc.end(), payload_.begin(), payload_.end());

  // calculate crc over combined data using static method from lumenheader
  return LumenHeader::calculate_crc8(data_for_crc);
}

// performs self-validation check (mainly for testing/debugging)
bool LumenPacket::is_valid() const {
  std::vector<uint8_t> packet_data = to_bytes(); // serialize self

  // basic size check
  if (packet_data.size() <
      LUMEN_HEADER_SIZE + 2) { // minimum size: header + crc + etx
    return false;
  }

  // check etx marker
  if (packet_data.back() != LUMEN_ETX) {
    return false;
  }

  // verify crc
  size_t crc_pos = packet_data.size() - 2; // crc is second to last byte
  std::vector<uint8_t> data_for_crc(packet_data.begin(),
                                    packet_data.begin() + crc_pos);
  uint8_t calculated_crc = LumenHeader::calculate_crc8(data_for_crc);
  uint8_t stored_crc = packet_data[crc_pos];

  return calculated_crc == stored_crc;
}

// static helper to find crc position
size_t LumenPacket::get_crc_position(const std::vector<uint8_t> &packet_data) {
  // crc is located right before etx byte (last byte)
  if (packet_data.size() < 2)
    return 0; // avoid underflow
  return packet_data.size() - 2;
}