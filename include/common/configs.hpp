// include/common/configs.hpp

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

// central configuration file for constants

// --- socket buffer sizes ---
constexpr size_t CLIENT_SOCK_BUF_SIZE = 1024;
constexpr size_t SERVER_SOCK_BUF_SIZE = 1024;

// --- lumen protocol delimiters ---
constexpr uint8_t LUMEN_STX = 0x02; // start of transmission marker byte
constexpr uint8_t LUMEN_ETX = 0x03; // end of transmission marker byte

// --- lumen header field positions/sizes ---
// defines the byte offsets and total size for fields within the fixed-size
// lumenheader
constexpr size_t LUMEN_STX_POS = 0;
constexpr size_t LUMEN_TYPE_POS = 1;
constexpr size_t LUMEN_PRIO_POS = 2;
constexpr size_t LUMEN_SEQ_POS = 3;
constexpr size_t LUMEN_TIMESTAMP_POS = 4;
constexpr size_t LUMEN_LEN_POS = 8;
constexpr size_t LUMEN_PAYLOAD_POS =
    10; // position where the payload data starts
constexpr size_t LUMEN_HEADER_SIZE =
    10; // total size of the lumenheader in bytes

// --- reliability manager constants (general) ---
constexpr int RELIABILITY_MAX_RETRIES =
    2; // max retransmission attempts (rover only)
constexpr std::chrono::milliseconds RELIABILITY_BASE_TIMEOUT{
    5000}; // initial timeout before first retry (rover only)

constexpr std::chrono::milliseconds RELIABILITY_CHECK_INTERVAL{
    1000}; // how often the reliabilitymanager timer checks for timeouts (rover
           // only)

// --- reliability manager constants (sequence tracking & cleanup) ---
static constexpr std::chrono::milliseconds NAK_DEBOUNCE_TIME{
    1000}; // minimum time between sending naks for the same sequence number
           // (rover only)

static constexpr std::chrono::milliseconds CLEANUP_INTERVAL{
    5000}; // how often reliabilitymanager cleans up old tracking data
static constexpr std::chrono::milliseconds SEQUENCE_RETAIN_TIME{
    30000}; // how long reliabilitymanager keeps records of received sequences

// --- lumen protocol constants ---
constexpr size_t MAX_FRAME_BUFFER_SIZE =
    4096; // maximum size allowed for the per-endpoint receive buffer

// --- udp client constants ---
constexpr std::chrono::milliseconds CLIENT_RETRY_DELAY{
    4000}; // delay before udpclient attempts to restart receive operations
           // after an error