// include/common/lumen_header.hpp

#pragma once

#include <cstdint>
#include <optional> // For std::optional return type
#include <vector>

/**
 * @class LumenHeader
 * @brief Represents the fixed-size header structure for LUMEN protocol packets.
 *
 * This class defines the metadata fields that precede the payload in a
 * LumenPacket. It handles serialization to and deserialization from a byte
 * stream according to the defined protocol structure. It also provides the
 * static CRC8 calculation method used by LumenPacket.
 *
 * Header Structure (10 bytes total):
 * - Byte 0: STX (Start Transmission Marker, 0x02)
 * - Byte 1: Message Type (See MessageType enum)
 * - Byte 2: Priority (See Priority enum)
 * - Byte 3: Sequence Number (uint8_t)
 * - Bytes 4-7: Timestamp (uint32_t, milliseconds since epoch, Big-Endian)
 * - Bytes 8-9: Payload Length (uint16_t, length of data following header,
 * Big-Endian)
 */
class LumenHeader {
public:
  /**
   * @enum MessageType
   * @brief Defines the type of payload contained within the LumenPacket.
   */
  enum class MessageType : uint8_t {
    ACK = 0,    ///< Acknowledgement packet.
    CMD = 1,    ///< Command message.
    DATA = 2,   ///< Generic data message (e.g., telemetry).
    VIDEO = 3,  ///< Video stream data
    STATUS = 4, ///< Status message.
    NAK = 5     ///< Negative Acknowledgement (retransmission request).
  };

  /**
   * @enum Priority
   * @brief Defines the priority level of the message.
   * Can be used for future QoS implementations (currently informational).
   */
  enum class Priority : uint8_t {
    LOW = 0,    ///< Low priority (e.g., bulk data, video).
    MEDIUM = 1, ///< Medium priority (e.g., telemetry, status).
    HIGH = 2    ///< High priority (e.g., commands, ACKs, NAKs).
  };

  /**
   * @brief Constructs a LumenHeader object.
   * @param type The MessageType of the packet.
   * @param prio The Priority level of the packet.
   * @param seq The sequence number (0-255) for this packet.
   * @param timestamp A 32-bit timestamp (e.g., milliseconds since epoch).
   * @param payload_length The length (in bytes) of the payload that will follow
   * this header.
   */
  LumenHeader(MessageType type, Priority prio, uint8_t seq, uint32_t timestamp,
              uint16_t payload_length);

  /**
   * @brief Attempts to parse a LumenHeader from the beginning of a byte vector.
   * Checks for STX marker and sufficient length. Validates enum ranges.
   * @param bytes The vector of received bytes, starting with the potential
   * header.
   * @return std::optional<LumenHeader> containing the parsed header if
   * successful, std::nullopt otherwise.
   */
  static std::optional<LumenHeader>
  from_bytes(const std::vector<uint8_t> &bytes);

  /**
   * @brief Serializes the LumenHeader object into a fixed-size byte vector
   * (LUMEN_HEADER_SIZE).
   * @return A std::vector<uint8_t> representing the serialized header.
   */
  std::vector<uint8_t> to_bytes() const;

  // --- Accessors ---
  MessageType get_type() const;        ///< Gets the message type.
  Priority get_priority() const;       ///< Gets the message priority.
  uint8_t get_sequence() const;        ///< Gets the sequence number.
  uint32_t get_timestamp() const;      ///< Gets the timestamp.
  uint16_t get_payload_length() const; ///< Gets the declared payload length.

  // --- Mutators ---
  /**
   * @brief Sets the sequence number.
   * @param seq The new sequence number.
   */
  void set_sequence(uint8_t seq);

  /**
   * @brief Sets the timestamp.
   * @param timestamp The new timestamp.
   */
  void set_timestamp(uint32_t timestamp);

  /**
   * @brief Sets the declared payload length.
   * Note: This should match the actual payload size when constructing a
   * LumenPacket.
   * @param length The declared payload length.
   */
  void set_payload_length(uint16_t length);

  /**
   * @brief Calculates the CRC8 checksum for a given byte vector.
   * Uses the CRC-8 polynomial 0x07 (x^8 + x^2 + x + 1).
   * This is used by LumenPacket to calculate the CRC over the header and
   * payload.
   * @param data The vector of bytes to calculate the CRC over.
   * @return The calculated 8-bit CRC value.
   */
  static uint8_t calculate_crc8(const std::vector<uint8_t> &data);

private:
  MessageType type_;        ///< Type of the message payload.
  Priority priority_;       ///< Priority level of the message.
  uint8_t sequence_;        ///< Sequence number (0-255).
  uint32_t timestamp_;      ///< Timestamp (e.g., milliseconds since epoch).
  uint16_t payload_length_; ///< Length of the payload following this header.
};