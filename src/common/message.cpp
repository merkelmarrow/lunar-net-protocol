// src/common/message.cpp

#include <iostream>
#include <memory>

#include "nlohmann/json.hpp"

#include "message.hpp"
#include "message_types.hpp"

namespace {
namespace nm = nlohmann;
}

bool Message::is_valid_json(const std::string &json_str) {
  try {
    nm::json::parse(json_str);
    return true;
  } catch (const nm::json::parse_error &) {
    return false;
  }
}

std::string Message::pretty_print(const std::string &json_str) {
  try {
    auto parsed = nm::json::parse(json_str);
    return parsed.dump(4); // 4 spaces for indentation
  } catch (const nm::json::parse_error &error) {
    throw std::runtime_error(std::string("Failed to parse JSON: ") +
                             error.what());
  }
}

std::unique_ptr<Message> Message::deserialise(const std::string &json_str) {
  try {
    nm::json j = nm::json::parse(json_str);

    // Enhanced validation of required fields
    if (!j.contains("msg_type")) {
      throw std::runtime_error("Invalid message: missing 'msg_type' field");
    }

    if (!j["msg_type"].is_string()) {
      throw std::runtime_error("Invalid message: 'msg_type' must be a string");
    }

    if (!j.contains("sender")) {
      throw std::runtime_error("Invalid message: missing 'sender' field");
    }

    if (!j["sender"].is_string()) {
      throw std::runtime_error("Invalid message: 'sender' must be a string");
    }

    std::string msg_type = j["msg_type"].get<std::string>();

// Use X-macro to check each message type
#define X(MessageType)                                                         \
  if (msg_type == MessageType::message_type()) {                               \
    return MessageType::from_json(j);                                          \
  }

    MESSAGE_TYPES_LIST // expands into if statements for each class

#undef X

        // only reach here if the message type is not one in the registry
        throw std::runtime_error("Unknown message type: " + msg_type);
  } catch (const nm::json::parse_error &error) {
    throw std::runtime_error(std::string("Failed to parse message: ") +
                             error.what());
  }
}

std::unique_ptr<Message> Message::create(const std::string &msg_type,
                                         const std::string &content,
                                         const std::string &sender) {
// Use X-macro to check each message type
#define X(MessageType)                                                         \
  if (msg_type == MessageType::message_type()) {                               \
    return MessageType::create_from_content(content, sender);                  \
  }

  MESSAGE_TYPES_LIST
#undef X

  throw std::runtime_error("Unknown message type: " + msg_type);
}