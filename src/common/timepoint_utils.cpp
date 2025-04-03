// src/common/timepoint_utils.cpp

#include "timepoint_utils.hpp"

#include <chrono>
#include <format> // requires c++20
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace tp_utils {

std::string tp_to_string(const std::chrono::system_clock::time_point &tp) {
  try {
    // format as iso 8601 date and time with timezone offset (requires c++20)
    return std::format("{:%FT%T%z}", tp);
  } catch (const std::format_error &e) {
    std::cerr << "[ERROR] Failed to format time_point: " << e.what()
              << std::endl;
    return "TimeFormatError";
  }
}

std::chrono::system_clock::time_point
string_to_tp(const std::string &time_str) {
  std::chrono::system_clock::time_point result;
  std::istringstream ss(time_str);

  try {
    // attempt parse common iso 8601 formats with timezone (requires c++20)
    // try format with numeric offset first (e.g., +0100)
    if (ss >> std::chrono::parse("%FT%T%z", result); !ss.fail()) {
      return result;
    }

    // reset stream state and try utc format 'z'
    ss.clear();  // clear fail bits
    ss.seekg(0); // reset position

    if (ss >> std::chrono::parse("%FT%T%Z", result); !ss.fail()) {
      return result;
    }

    // if all attempts fail, throw error
    throw std::runtime_error("Timestamp format not recognized or parse failed "
                             "(tried %FT%T%z and %FT%T%Z).");

  } catch (const std::exception &e) {
    // catch errors from parsing or explicit throw
    std::cerr << "[ERROR] Failed to parse timestamp string '" << time_str
              << "' using std::chrono::parse. Error: " << e.what() << std::endl;
    std::cerr << "[WARN] Using current time as fallback." << std::endl;
    return std::chrono::system_clock::now(); // return current time as fallback
  }
}

} // namespace tp_utils