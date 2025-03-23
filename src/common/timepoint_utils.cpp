#include "timepoint_utils.hpp"
#include <chrono>
#include <format>
#include <iostream>
#include <sstream>

namespace tp_utils {
std::string tp_to_string(const std::chrono::system_clock::time_point &tp) {
  return std::format("{:%FT%T%z}", tp);
}

std::chrono::system_clock::time_point
string_to_tp(const std::string &time_str) {

  std::istringstream ss(time_str);
  std::chrono::system_clock::time_point result;

  ss >> std::chrono::parse("%FT%T%z", result);

  if (ss.fail()) {
    std::cerr << "Failed to parse timestamp. " << time_str << std::endl;
    std::cerr << "Using current time." << std::endl;
    return std::chrono::system_clock::now();
  }

  return result;
}
} // namespace tp_utils