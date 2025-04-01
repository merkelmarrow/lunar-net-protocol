// include/common/timepoint_utils.hpp

#pragma once

#include <chrono> // For std::chrono::system_clock::time_point
#include <string> // For std::string

/**
 * @namespace tp_utils
 * @brief Provides utility functions for converting between
 * std::chrono::system_clock::time_point and string representations, typically
 * used for message timestamps.
 */
namespace tp_utils {

/**
 * @brief Converts a system_clock::time_point to an ISO 8601 formatted string
 * with timezone. Example format: "YYYY-MM-DDTHH:MM:SS+ZZZZ" (e.g.,
 * "2023-10-27T10:30:00+0100")
 * @param tp The time_point to convert.
 * @return std::string The formatted timestamp string.
 */
std::string tp_to_string(const std::chrono::system_clock::time_point &tp);

/**
 * @brief Converts an ISO 8601 formatted timestamp string (with timezone) back
 * to a system_clock::time_point. Expects format like
 * "YYYY-MM-DDTHH:MM:SS+ZZZZ".
 * @param time_str The formatted timestamp string to parse.
 * @return std::chrono::system_clock::time_point The parsed time_point.
 * @note If parsing fails, logs an error and returns
 * std::chrono::system_clock::now() as a fallback.
 */
std::chrono::system_clock::time_point string_to_tp(const std::string &time_str);

} // namespace tp_utils