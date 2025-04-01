// src/common/message.cpp

#include "message.hpp"
#include "message_types.hpp" // Includes headers for all concrete message types & the X-macro list

#include "nlohmann/json.hpp" // For JSON parsing

#include <iostream>
#include <map>       // Include map for TelemetryMessage
#include <memory>    // For std::unique_ptr
#include <stdexcept> // For std::runtime_error
#include <string>    // For std::string manipulation in create()

namespace nm = nlohmann;

// Static utility function to check if a string is valid JSON.
bool Message::is_valid_json(const std::string &json_str) {
  try {
    // Attempt to parse. If it doesn't throw, it's valid JSON structure.
    nm::json::parse(json_str);
    return true;
  } catch (const nm::json::parse_error &) {
    // Parsing failed, not valid JSON.
    return false;
  }
}

// Static utility function to format a JSON string with indentation.
std::string Message::pretty_print(const std::string &json_str) {
  try {
    auto parsed_json = nm::json::parse(json_str);
    // Use dump() with an indentation value (e.g., 4 spaces).
    return parsed_json.dump(4);
  } catch (const nm::json::parse_error &error) {
    // Re-throw as a runtime_error for consistent error handling upstream.
    throw std::runtime_error(
        std::string("Failed to parse JSON for pretty printing: ") +
        error.what());
  }
}

// Static factory method to deserialize a JSON string into a specific Message
// subclass.
std::unique_ptr<Message> Message::deserialise(const std::string &json_str) {
  try {
    nm::json j = nm::json::parse(json_str);

    // --- Basic Structure Validation ---
    if (!j.contains("msg_type") || !j["msg_type"].is_string()) {
      throw std::runtime_error("Invalid message JSON: missing or invalid "
                               "'msg_type' field (must be a string)");
    }
    // Ensure 'sender' exists here as a common requirement before dispatching.
    if (!j.contains("sender") || !j["sender"].is_string()) {
      throw std::runtime_error("Invalid message JSON: missing or invalid "
                               "'sender' field (must be a string)");
    }
    // --- End Validation ---

    std::string msg_type = j["msg_type"].get<std::string>();

// --- Factory Dispatch using X-Macro ---
#define X(MessageType)                                                         \
  if (msg_type == MessageType::message_type()) {                               \
    /* Delegate deserialization to the static */                               \
    /* from_json method of the specific subclass */                            \
    return MessageType::from_json(j);                                          \
  }

    MESSAGE_TYPES_LIST // Expands the #define X block for each message type

#undef X // Undefine the macro after use

        // If msg_type didn't match any known type in the list:
        throw std::runtime_error(
            "Unknown message type encountered during deserialization: " +
            msg_type);

  } catch (const nm::json::parse_error &error) {
    // Error during initial JSON parsing.
    throw std::runtime_error(std::string("Failed to parse message JSON: ") +
                             error.what());
  } catch (const std::runtime_error &error) {
    // Catch errors thrown by subclass from_json methods or validation checks.
    throw; // Re-throw the specific error (e.g., missing fields).
  } catch (const std::exception &error) {
    // Catch any other potential exceptions during deserialization.
    throw std::runtime_error(
        std::string(
            "An unexpected error occurred during message deserialization: ") +
        error.what());
  }
}