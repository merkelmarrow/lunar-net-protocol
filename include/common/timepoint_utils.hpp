// include/common/timepoint_utils.hpp

#pragma once

#include <chrono>
#include <string>

namespace tp_utils {
std::string tp_to_string(const std::chrono::system_clock::time_point &tp);
std::chrono::system_clock::time_point string_to_tp(const std::string &time_str);
} // namespace tp_utils