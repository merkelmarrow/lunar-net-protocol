// src/common/message.cpp

#include <iostream>
#include <memory>

#include "nlohmann/json.hpp"

#include "message.hpp"
#include "message_types.hpp"

namespace nm = nlohmann;

bool Message::is_valid_json(const std::string &json_str) {
  try {
    auto result = nm::json::parse(json_str);
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

  if (msg_type == BasicMessage::message_type()) {
    return BasicMessage::create_from_content(content, sender);
  } else if (msg_type == CommandMessage::message_type()) {
    // parse content as command and params
    size_t delimiter_pos = content.find(':');
    std::string command, params;

    if (delimiter_pos != std::string::npos) {
      command = content.substr(0, delimiter_pos);
      params = content.substr(delimiter_pos + 1);
    } else {
      command = content;
      params = "";
    }

    return CommandMessage::create_from_content(command, params, sender);
  } else if (msg_type == StatusMessage::message_type()) {
    // default to NOMINAL status if not specified
    return StatusMessage::create_from_content(StatusMessage::StatusLevel::OK,
                                              content, sender);
  } else if (msg_type == TelemetryMessage::message_type()) {
    // for telemetry, we need a proper map, but can't parse arbitrary content
    // default to empty readings
    std::map<std::string, double> empty_readings;
    return TelemetryMessage::create_from_content(empty_readings, sender);
  }

  throw std::runtime_error("Unknown message type: " + msg_type);
}