// src/common/timepoint_utils.cpp

#include "timepoint_utils.hpp"

#include <chrono>    // For std::chrono types and parse
#include <format>    // For std::format (requires C++20)
#include <iostream>  // For error logging (cerr)
#include <sstream>   // For string stream used by std::chrono::parse
#include <stdexcept> // For std::runtime_error

namespace tp_utils {

std::string tp_to_string(const std::chrono::system_clock::time_point &tp) {
  try {
    // Format as ISO 8601 date and time with timezone offset (requires C++20)
    return std::format("{:%FT%T%z}", tp);
  } catch (const std::format_error &e) {
    // Log error if formatting fails
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
    // Attempt to parse common ISO 8601 formats with timezone (requires C++20)
    // Try format with numeric offset first (e.g., +0100)
    if (ss >> std::chrono::parse("%FT%T%z", result); !ss.fail()) {
      return result;
    }

    // Reset stream state and try UTC format 'Z'
    ss.clear();  // Clear fail bits
    ss.seekg(0); // Reset position to beginning

    if (ss >> std::chrono::parse("%FT%T%Z", result); !ss.fail()) {
      return result;
    }

    // If all attempts fail, throw an error
    throw std::runtime_error("Timestamp format not recognized or parse failed "
                             "(tried %FT%T%z and %FT%T%Z).");

  } catch (const std::exception &e) {
    // Catch errors from parsing or the explicit throw above
    std::cerr << "[ERROR] Failed to parse timestamp string '" << time_str
              << "' using std::chrono::parse. Error: " << e.what() << std::endl;
    std::cerr << "[WARN] Using current time as fallback." << std::endl;
    return std::chrono::system_clock::now(); // Return current time as a
                                             // fallback
  }
}

} // namespace tp_utils