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

// static factory method
// Creates a message object from basic string inputs using if-else if structure.
std::unique_ptr<Message> Message::create(const std::string &msg_type,
                                         const std::string &content,
                                         const std::string &sender) {

  if (msg_type == BasicMessage::message_type()) {
    // BasicMessage::create_from_content(const std::string& content, const
    // std::string& sender)
    return BasicMessage::create_from_content(content, sender);
  } else if (msg_type == CommandMessage::message_type()) {
    // CommandMessage::create_from_content(const std::string& command, const
    // std::string& params, const std::string& sender) Parse 'content' into
    // 'command' and 'params' (simple split on first ':')
    size_t delimiter_pos = content.find(':');
    std::string command = (delimiter_pos != std::string::npos)
                              ? content.substr(0, delimiter_pos)
                              : content;
    std::string params = (delimiter_pos != std::string::npos)
                             ? content.substr(delimiter_pos + 1)
                             : "";
    return CommandMessage::create_from_content(command, params, sender);
  } else if (msg_type == StatusMessage::message_type()) {
    // StatusMessage::create_from_content(StatusLevel level, const std::string&
    // description, const std::string& sender) Assume 'content' is the
    // description, use a default status level.
    return StatusMessage::create_from_content(StatusMessage::StatusLevel::OK,
                                              content, sender);
  } else if (msg_type == TelemetryMessage::message_type()) {
    // TelemetryMessage::create_from_content(const std::map<std::string,
    // double>& readings, const std::string& sender) Cannot realistically parse
    // a map from the single 'content' string here. Create with empty readings
    // as a fallback.
    std::cerr << "[WARN] Message::create cannot parse Telemetry content from "
                 "string. Creating with empty readings for sender "
              << sender << "." << std::endl;
    std::map<std::string, double> empty_readings;
    return TelemetryMessage::create_from_content(empty_readings, sender);
  }
  // Add else if blocks here for any other message types defined in
  // MESSAGE_TYPES_LIST

  // If msg_type didn't match any known type
  throw std::runtime_error(
      "Message::create - Unknown or unsupported message type: " + msg_type);
}