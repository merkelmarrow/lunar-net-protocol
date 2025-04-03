// include/common/configs.hpp

#pragma once

#include <chrono>  // For std::chrono::milliseconds
#include <cstddef> // For std::size_t
#include <cstdint> // For uint8_t etc.

/**
 * @file configs.hpp
 * @brief Central configuration file for constants used in the
 * lunar-net-protocol.
 */

// --- Socket Buffer Sizes ---
// Defines the size of the underlying receive buffers for UDP sockets.
// Larger buffers can handle larger datagrams but consume more memory.
constexpr size_t CLIENT_SOCK_BUF_SIZE =
    1024; ///< Receive buffer size for UdpClient.
constexpr size_t SERVER_SOCK_BUF_SIZE =
    1024; ///< Receive buffer size for UdpServer.

// --- LUMEN Protocol Delimiters ---
constexpr uint8_t LUMEN_STX = 0x02; ///< Start of Transmission marker byte.
constexpr uint8_t LUMEN_ETX = 0x03; ///< End of Transmission marker byte.

// --- LUMEN Header Field Positions/Sizes ---
// Defines the byte offsets and total size for fields within the fixed-size
// LumenHeader.
constexpr size_t LUMEN_STX_POS = 0;  ///< Position of STX marker.
constexpr size_t LUMEN_TYPE_POS = 1; ///< Position of Message Type byte.
constexpr size_t LUMEN_PRIO_POS = 2; ///< Position of Priority byte.
constexpr size_t LUMEN_SEQ_POS = 3;  ///< Position of Sequence Number byte.
constexpr size_t LUMEN_TIMESTAMP_POS =
    4; ///< Starting position of 4-byte Timestamp.
constexpr size_t LUMEN_LEN_POS =
    8; ///< Starting position of 2-byte Payload Length.
constexpr size_t LUMEN_PAYLOAD_POS =
    10; ///< Position where the payload data starts (after the header).
constexpr size_t LUMEN_HEADER_SIZE =
    10; ///< Total size of the LumenHeader in bytes.

// --- Reliability Manager Constants (General) ---
constexpr int RELIABILITY_MAX_RETRIES =
    2; ///< Max retransmission attempts (Rover only).
constexpr std::chrono::milliseconds RELIABILITY_BASE_TIMEOUT{
    5000}; ///< Initial timeout before first retry (Rover only). Timeout doubles
           ///< with each retry.
constexpr std::chrono::milliseconds RELIABILITY_CHECK_INTERVAL{
    1000}; ///< How often the ReliabilityManager timer checks for timeouts
           ///< (Rover only).

// --- Reliability Manager Constants (Sequence Tracking & Cleanup) ---
static constexpr std::chrono::milliseconds NAK_DEBOUNCE_TIME{
    1000}; ///< Minimum time between sending NAKs for the same sequence number
           ///< (Rover only).
static constexpr std::chrono::milliseconds CLEANUP_INTERVAL{
    5000}; ///< How often ReliabilityManager cleans up old tracking data.
static constexpr std::chrono::milliseconds SEQUENCE_RETAIN_TIME{
    30000}; ///< How long ReliabilityManager keeps records of received
            ///< sequences.

// --- Lumen Protocol Constants ---
constexpr size_t MAX_FRAME_BUFFER_SIZE =
    4096; ///< Maximum size allowed for the per-endpoint receive buffer in
          ///< LumenProtocol before clearing. Prevents excessive memory use from
          ///< fragmented/corrupt data.

// --- UDP Client Constants ---
constexpr std::chrono::milliseconds CLIENT_RETRY_DELAY{
    4000}; ///< Delay before UdpClient attempts to restart receive operations
           ///< after an error.