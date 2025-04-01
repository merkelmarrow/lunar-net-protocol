// src/common/lumen_packet.cpp

#include "lumen_packet.hpp"
#include "configs.hpp"      // For LUMEN_HEADER_SIZE, LUMEN_STX, LUMEN_ETX etc.
#include "lumen_header.hpp" // Included for LumenHeader definition and CRC function
#include <iostream>
#include <optional>
#include <vector>

// Constructor: Takes a header and payload, ensuring payload length is correctly
// set in the header.
LumenPacket::LumenPacket(const LumenHeader &header,
                         const std::vector<uint8_t> &payload)
    : header_(header), payload_(payload) {
  // Ensure the header reflects the actual payload size provided.
  header_.set_payload_length(static_cast<uint16_t>(payload_.size()));
}

// Factory method: Attempts to parse a LumenPacket from a byte stream.
std::optional<LumenPacket>
LumenPacket::from_bytes(const std::vector<uint8_t> &bytes) {
  // Step 1: Try to parse the header first.
  // LumenHeader::from_bytes checks for minimum size and STX marker.
  auto header_opt = LumenHeader::from_bytes(bytes);
  if (!header_opt) {
    // Not enough bytes for a header or STX is missing/incorrect.
    return std::nullopt;
  }
  LumenHeader header = *header_opt;

  // Step 2: Calculate the expected total size based on the header's payload
  // length. Total Size = Header Size + Payload Length + CRC Byte + ETX Byte
  size_t expected_size = LUMEN_HEADER_SIZE + header.get_payload_length() + 2;

  // Step 3: Check if we have enough bytes in the buffer for the full declared
  // packet.
  if (bytes.size() < expected_size) {
    // Not enough data received yet for this packet.
    return std::nullopt;
  }

  // Step 4: Check for the End Transmission (ETX) marker at the expected
  // position.
  if (bytes[expected_size - 1] != LUMEN_ETX) {
    std::cerr << "[ERROR] LumenPacket::from_bytes: Missing or incorrect ETX "
                 "marker. Expected size: "
              << expected_size << std::endl;
    return std::nullopt;
  }

  // Step 5: Extract the payload data.
  std::vector<uint8_t> payload(bytes.begin() + LUMEN_PAYLOAD_POS,
                               bytes.begin() + LUMEN_PAYLOAD_POS +
                                   header.get_payload_length());

  // Step 6: Verify the CRC checksum.
  // Extract the CRC byte stored in the packet (byte before ETX).
  uint8_t stored_crc = bytes[expected_size - 2];

  // Calculate the CRC based on the header and payload bytes (all bytes *before*
  // the CRC byte itself).
  std::vector<uint8_t> data_for_crc(bytes.begin(),
                                    bytes.begin() + expected_size - 2);
  uint8_t calculated_crc = LumenHeader::calculate_crc8(data_for_crc);

  if (calculated_crc != stored_crc) {
    std::cerr << "[ERROR] LumenPacket::from_bytes: CRC mismatch! Calculated=0x"
              << std::hex << static_cast<int>(calculated_crc) << ", Stored=0x"
              << static_cast<int>(stored_crc) << std::dec
              << ". Packet seq: " << static_cast<int>(header.get_sequence())
              << std::endl;
    // CRC failed, packet is corrupt.
    return std::nullopt;
  }

  // Step 7: All checks passed, create and return the LumenPacket object.
  // Use the parsed header (header) and extracted payload.
  return LumenPacket(header, payload);
}

const LumenHeader &LumenPacket::get_header() const { return header_; }

const std::vector<uint8_t> &LumenPacket::get_payload() const {
  return payload_;
}

// Serializes the LumenPacket (header + payload) into a byte vector suitable for
// transmission.
std::vector<uint8_t> LumenPacket::to_bytes() const {
  // Start with the serialized header bytes.
  std::vector<uint8_t> packet_bytes = header_.to_bytes();

  // Append the payload bytes.
  packet_bytes.insert(packet_bytes.end(), payload_.begin(), payload_.end());

  // Calculate CRC over the combined header and payload.
  uint8_t crc = calculate_packet_crc(); // Use internal helper
  packet_bytes.push_back(crc);

  // Append the End Transmission (ETX) marker.
  packet_bytes.push_back(LUMEN_ETX);

  return packet_bytes;
}

// Returns the total expected size of the packet on the wire.
size_t LumenPacket::total_size() const {
  // Header Size + Payload Size + CRC (1 byte) + ETX (1 byte)
  return LUMEN_HEADER_SIZE + payload_.size() + 2;
}

// Calculates the CRC8 checksum over the packet's header and payload.
uint8_t LumenPacket::calculate_packet_crc() const {
  std::vector<uint8_t> data_for_crc;
  data_for_crc.reserve(LUMEN_HEADER_SIZE + payload_.size()); // Reserve space

  // Get header bytes.
  std::vector<uint8_t> header_bytes = header_.to_bytes();
  data_for_crc.insert(data_for_crc.end(), header_bytes.begin(),
                      header_bytes.end());

  // Append payload bytes.
  data_for_crc.insert(data_for_crc.end(), payload_.begin(), payload_.end());

  // Calculate CRC over the combined data using the static method from
  // LumenHeader.
  return LumenHeader::calculate_crc8(data_for_crc);
}

// Performs a self-validation check (primarily for testing/debugging).
// Serializes the packet and then validates STX, ETX, and CRC.
bool LumenPacket::is_valid() const {
  std::vector<uint8_t> packet_data = to_bytes(); // Serialize self

  // Basic size check
  if (packet_data.size() <
      LUMEN_HEADER_SIZE + 2) { // Minimum size: Header + CRC + ETX
    return false;
  }

  // Check ETX marker
  if (packet_data.back() != LUMEN_ETX) {
    return false;
  }

  // Verify CRC
  size_t crc_pos = packet_data.size() - 2; // CRC is second to last byte
  std::vector<uint8_t> data_for_crc(packet_data.begin(),
                                    packet_data.begin() + crc_pos);
  uint8_t calculated_crc = LumenHeader::calculate_crc8(data_for_crc);
  uint8_t stored_crc = packet_data[crc_pos];

  return calculated_crc == stored_crc;
}

// Static helper to find CRC position (useful if needed externally, maybe not
// necessary). Kept from original code.
size_t LumenPacket::get_crc_position(const std::vector<uint8_t> &packet_data) {
  // CRC is located right before the ETX byte (last byte).
  if (packet_data.size() < 2)
    return 0; // Avoid underflow
  return packet_data.size() - 2;
}