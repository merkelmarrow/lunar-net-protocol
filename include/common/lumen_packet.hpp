// include/common/lumen_packet.hpp

#pragma once

#include "lumen_header.hpp" // Requires full definition of LumenHeader
#include <cstdint>
#include <optional> // For std::optional return type
#include <vector>

/**
 * @class LumenPacket
 * @brief Represents a complete LUMEN protocol data unit, including the header
 * and payload.
 *
 * This class encapsulates the structure of a packet as defined by the LUMEN
 * protocol. It provides methods for creating packets, serializing them into
 * byte vectors for transmission, and deserializing them from received byte
 * streams. It also includes CRC validation logic.
 *
 * The packet structure is generally:
 * [LumenHeader (10 bytes)] + [Payload (variable length)] + [CRC (1 byte)] +
 * [ETX (1 byte)]
 */
class LumenPacket {
public:
  /**
   * @brief Constructs a LumenPacket from a header and payload.
   * Automatically updates the payload length field in the provided header
   * object.
   * @param header A LumenHeader object containing packet metadata. The payload
   * length will be updated.
   * @param payload A vector of bytes representing the message payload.
   */
  LumenPacket(const LumenHeader &header, const std::vector<uint8_t> &payload);

  /**
   * @brief Attempts to parse a LumenPacket from the beginning of a byte vector.
   * Checks for STX marker, sufficient length, valid header fields, ETX marker,
   * and correct CRC checksum.
   * @param bytes The vector of received bytes. It should contain at least one
   * complete packet at the beginning.
   * @return std::optional<LumenPacket> containing the parsed packet if
   * successful, std::nullopt otherwise. If successful, the input `bytes` vector
   * is not modified; the caller is responsible for removing the processed bytes
   * based on `total_size()`.
   */
  static std::optional<LumenPacket>
  from_bytes(const std::vector<uint8_t> &bytes);

  /**
   * @brief Serializes the LumenPacket (header, payload, CRC, ETX) into a byte
   * vector.
   * @return A std::vector<uint8_t> ready for transmission.
   */
  std::vector<uint8_t> to_bytes() const;

  /**
   * @brief Gets a constant reference to the packet's header.
   * @return const LumenHeader&
   */
  const LumenHeader &get_header() const;

  /**
   * @brief Gets a constant reference to the packet's payload.
   * @return const std::vector<uint8_t>&
   */
  const std::vector<uint8_t> &get_payload() const;

  /**
   * @brief Performs a self-validation check by serializing and checking STX,
   * ETX, and CRC. Mainly useful for debugging or testing.
   * @return True if the packet appears valid based on its own serialization,
   * false otherwise.
   */
  bool is_valid() const;

  /**
   * @brief Calculates the total size of the serialized packet in bytes.
   * Size = Header Size + Payload Size + CRC Size (1) + ETX Size (1).
   * @return The total size of the packet when serialized.
   */
  size_t total_size() const;

  /**
   * @brief Static helper to determine the expected position of the CRC byte
   * within a packet data buffer. Note: Validity of packet_data length is not
   * checked here.
   * @param packet_data A vector containing serialized packet data.
   * @return The index of the CRC byte (size - 2). Returns 0 if size < 2.
   */
  static size_t get_crc_position(const std::vector<uint8_t> &packet_data);

private:
  LumenHeader header_;           ///< The header part of the packet.
  std::vector<uint8_t> payload_; ///< The payload data of the packet.

  /**
   * @brief Calculates the CRC8 checksum over the header and payload of this
   * packet instance.
   * @return The calculated 8-bit CRC value.
   */
  uint8_t calculate_packet_crc() const;
};