// src/common/message.cpp

#include "message.hpp"
#include "message_types.hpp" // includes headers for all concrete message types & the x-macro list

#include "nlohmann/json.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace nm = nlohmann;

// static utility function to check if string is valid json
bool Message::is_valid_json(const std::string &json_str) {
  try {
    nm::json::parse(json_str); // attempt parse
    return true;
  } catch (const nm::json::parse_error &) {
    return false; // parsing failed
  }
}

// static utility function to format json string with indentation
std::string Message::pretty_print(const std::string &json_str) {
  try {
    auto parsed_json = nm::json::parse(json_str);
    return parsed_json.dump(4); // use dump() with indentation
  } catch (const nm::json::parse_error &error) {
    // re-throw for consistent error handling upstream
    throw std::runtime_error(
        std::string("Failed to parse JSON for pretty printing: ") +
        error.what());
  }
}

// static factory method to deserialize json string into specific message
// subclass
std::unique_ptr<Message> Message::deserialise(const std::string &json_str) {
  try {
    nm::json j = nm::json::parse(json_str);

    // --- basic structure validation ---
    if (!j.contains("msg_type") || !j["msg_type"].is_string()) {
      throw std::runtime_error("Invalid message JSON: missing or invalid "
                               "'msg_type' field (must be a string)");
    }
    // ensure 'sender' exists before dispatching
    if (!j.contains("sender") || !j["sender"].is_string()) {
      throw std::runtime_error("Invalid message JSON: missing or invalid "
                               "'sender' field (must be a string)");
    }
    // --- end validation ---

    std::string msg_type = j["msg_type"].get<std::string>();

// --- factory dispatch using x-macro ---
// expands the #define x block for each message type
#define X(MessageType)                                                         \
  if (msg_type == MessageType::message_type()) {                               \
    /* delegate deserialization to static from_json method of subclass */      \
    return MessageType::from_json(j);                                          \
  }

    MESSAGE_TYPES_LIST

#undef X // undefine macro after use

    // if msg_type didn't match any known type
    throw std::runtime_error(
        "Unknown message type encountered during deserialization: " + msg_type);

  } catch (const nm::json::parse_error &error) {
    // error during initial json parsing
    throw std::runtime_error(std::string("Failed to parse message JSON: ") +
                             error.what());
  } catch (const std::runtime_error &error) {
    // catch errors thrown by subclass from_json methods or validation checks
    throw; // re-throw specific error
  } catch (const std::exception &error) {
    // catch any other potential exceptions
    throw std::runtime_error(
        std::string(
            "An unexpected error occurred during message deserialization: ") +
        error.what());
  }
}