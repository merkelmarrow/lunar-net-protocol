// include/common/configs.hpp

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

constexpr size_t CLIENT_SOCK_BUF_SIZE = 1024;
constexpr size_t SERVER_SOCK_BUF_SIZE = 1024;

constexpr uint8_t LUMEN_STX = 0x02;
constexpr uint8_t LUMEN_ETX = 0x03;

constexpr size_t LUMEN_STX_POS = 0;
constexpr size_t LUMEN_TYPE_POS = 1;
constexpr size_t LUMEN_PRIO_POS = 2;
constexpr size_t LUMEN_SEQ_POS = 3;
constexpr size_t LUMEN_TIMESTAMP_POS = 4;
constexpr size_t LUMEN_LEN_POS = 8;
constexpr size_t LUMEN_PAYLOAD_POS = 9;
constexpr size_t LUMEN_HEADER_SIZE = 9;

constexpr int RELIABILITY_MAX_RETRIES = 5;
constexpr std::chrono::milliseconds RELIABILITY_BASE_TIMEOUT{5000};
constexpr std::chrono::milliseconds RELIABILITY_CHECK_INTERVAL{1000};

constexpr size_t SACK_WINDOW_SIZE = 32;

constexpr size_t MAX_FRAME_BUFFER_SIZE = 4096;

constexpr std::chrono::milliseconds CLIENT_RETRY_DELAY{4000};