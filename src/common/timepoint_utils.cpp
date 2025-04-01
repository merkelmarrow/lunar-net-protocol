// src/common/timepoint_utils.cpp

#include "timepoint_utils.hpp"

#include <chrono>
// Use <format> if available (C++20) for cleaner formatting.
// Ensure CMakeLists.txt sets CMAKE_CXX_STANDARD to 20 or higher.
#if __cplusplus >= 202002L && __has_include(<format>)
#include <format>
#else
// Fallback for older compilers (less elegant)
#include <iomanip> // For std::put_time
#include <locale>  // Required by some put_time implementations
#endif
#include <iostream> // For error logging (cerr)
#include <sstream>  // For string stream parsing

namespace tp_utils {

std::string tp_to_string(const std::chrono::system_clock::time_point &tp) {
// Use C++20 std::format if available
#if __cplusplus >= 202002L && __has_include(<format>)
  // Format as ISO 8601 date and time with timezone offset
  return std::format("{:%FT%T%z}", tp);
#else
  // Fallback using put_time (requires C++11 or later)
  std::time_t time = std::chrono::system_clock::to_time_t(tp);
  std::tm tm_buf;
// Use localtime_s (safer) on Windows if available, otherwise localtime_r or
// localtime
#ifdef _WIN32
  if (localtime_s(&tm_buf, &time) != 0) { /* Handle error */
    return "TimeFormatError";
  }
#else
  // POSIX-like systems usually have localtime_r
  if (localtime_r(&time, &tm_buf) == nullptr) { /* Handle error */
    return "TimeFormatError";
  }
#endif

  std::ostringstream ss;
  // Format: YYYY-MM-DDTHH:MM:SS+ZZZZ (requires manual timezone formatting)
  // Note: std::put_time's %z might not be universally supported or consistent.
  // This example uses std::strftime which has better %z support generally.
  char buffer[100];
  if (std::strftime(buffer, sizeof(buffer), "%FT%T%z", &tm_buf)) {
    return std::string(buffer);
  } else {
    return "TimeFormatError";
  }
#endif
}

std::chrono::system_clock::time_point
string_to_tp(const std::string &time_str) {
  std::chrono::system_clock::time_point result;
  std::istringstream ss(time_str);

// Set locale to ensure consistent parsing if needed (usually C locale is fine
// for ISO format) ss.imbue(std::locale("C"));

// Use std::chrono::parse (C++20) if available for better error handling and
// format support
#if __cplusplus >= 202002L && __has_include(<chrono>) && defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
  try {
    // Attempt to parse common ISO 8601 formats with timezone
    // Note: %Z (timezone name) is generally harder to parse reliably than %z
    // (offset). Use %z for offset like +0100 or Z for UTC.
    if (ss >> std::chrono::parse("%FT%T%z", result);
        !ss.fail()) { // Try with offset first
      return result;
    }
    // Reset stream state and try UTC format 'Z'
    ss.clear();
    ss.str(time_str);
    if (ss >> std::chrono::parse("%FT%T%Z", result);
        !ss.fail()) { // Try with 'Z'
      return result;
    }
    // Add more formats if needed

    // If all attempts fail
    throw std::runtime_error(
        "Timestamp format not recognized or parse failed.");

  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Failed to parse timestamp string '" << time_str
              << "' using std::chrono::parse. Error: " << e.what() << std::endl;
    std::cerr << "[WARN] Using current time as fallback." << std::endl;
    return std::chrono::system_clock::now();
  }

#else
  // Fallback using std::get_time (less robust, sensitive to locale and format
  // issues)
  std::tm tm = {};
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S"); // Parse main part

  // Attempt to parse timezone offset manually (example, very basic)
  // This part is complex and fragile with get_time. std::chrono::parse is
  // preferred. A library like `date.h` by Howard Hinnant would be a better
  // pre-C++20 solution. For simplicity in fallback, we might ignore timezone or
  // assume UTC if not C++20.

  if (ss.fail()) {
    std::cerr << "[ERROR] Failed to parse timestamp string '" << time_str
              << "' using std::get_time." << std::endl;
    std::cerr << "[WARN] Using current time as fallback." << std::endl;
    return std::chrono::system_clock::now();
  }

  // Convert std::tm to time_point (assuming local time if timezone wasn't
  // parsed)
  // Note: timegm converts assuming UTC, mktime assuming local time. Behaviour
  // depends on what the timestamp represents. If timestamps are always UTC, use
  // a portable timegm equivalent or date.h library. If local, use mktime. Let's
  // assume local for this fallback example.
  std::time_t tt = std::mktime(&tm);
  if (tt == -1) {
    std::cerr << "[ERROR] Failed to convert parsed time components to time_t "
                 "for string '"
              << time_str << "'" << std::endl;
    std::cerr << "[WARN] Using current time as fallback." << std::endl;
    return std::chrono::system_clock::now();
  }
  result = std::chrono::system_clock::from_time_t(tt);
  return result;
#endif
}

} // namespace tp_utils