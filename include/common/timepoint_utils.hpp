// include/common/timepoint_utils.hpp

#pragma once

#include <chrono>
#include <string>

// provides utility functions for converting time_point and string
// representations
namespace tp_utils {

// converts a system_clock::time_point to an iso 8601 formatted string with
// timezone
std::string tp_to_string(const std::chrono::system_clock::time_point &tp);

// converts an iso 8601 formatted timestamp string back to a
// system_clock::time_point
std::chrono::system_clock::time_point string_to_tp(const std::string &time_str);

} // namespace tp_utils