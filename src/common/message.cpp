// src/common/message.cpp

#include <iostream>
#include <memory>

#include "message.hpp"

namespace {
namespace nm = nlohmann;
}

// Registry implementation
std::unordered_map<std::string, MessageFactory> &
Message::get_message_registry() {
  static std::unordered_map<std::string, MessageFactory> registry;
  return registry;
}

void Message::register_message_type(const std::string &msg_type,
                                    MessageFactory factory) {
  auto &registry = get_message_registry();
  registry[msg_type] = std::move(factory);
  std::cout << "Registered message type: " << msg_type << ".\n";
}

// Message base class static methods
std::unique_ptr<Message> Message::deserialise(const std::string &json_str) {
  try {
    nm::json j = nm::json::parse(json_str);

    if (!j.contains("msg_type")) {
      throw std::runtime_error(
          "Unable to interpret message. Missing 'msg_type' field");
    }

    std::string msg_type = j["msg_type"].get<std::string>();
    auto &registry = get_message_registry();

    if (registry.find(msg_type) != registry.end()) {
      return registry[msg_type](j);
    } else {
      throw std::runtime_error("Unknown message type: " + msg_type);
    }
  } catch (const nm::json::parse_error &e) {
    throw std::runtime_error(std::string("Failed to parse message: ") +
                             e.what());
  }
}

bool Message::is_valid_json(const std::string &json_str) {
  try {
    nm::json::parse(json_str);
    return true;
  } catch (const nm::json::parse_error &) {
    return false;
  }
}