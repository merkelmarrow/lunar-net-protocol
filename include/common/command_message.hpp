// include/common/command_message.hpp

#pragma once

#include "lumen_header.hpp"
#include "message.hpp"
#include "timepoint_utils.hpp"

#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace nm = nlohmann;

// represents a command sent between nodes
class CommandMessage : public Message {
public:
  CommandMessage(const std::string &command, const std::string &params,
                 const std::string &sender)
      : Message(sender), command_(command), params_(params) {}

  std::string serialise() const override {
    std::string formatted_time = tp_utils::tp_to_string(timestamp_);
    nm::json j = {{"msg_type", get_type()},
                  {"command", command_},
                  {"params", params_},
                  {"sender", sender_},
                  {"timestamp", formatted_time}};
    return j.dump();
  }

  std::string get_type() const override { return message_type(); }

  LumenHeader::Priority get_lumen_priority() const override {
    return LumenHeader::Priority::HIGH; // commands are high priority
  }

  LumenHeader::MessageType get_lumen_type() const override {
    return LumenHeader::MessageType::CMD; // maps to command type in protocol
  }

  // --- getters ---
  const std::string &get_command() const { return command_; }

  const std::string &get_params() const { return params_; }

  // --- static factory methods ---

  static std::string message_type() { return "CommandMessage"; }

  static std::unique_ptr<CommandMessage> from_json(const nm::json &j) {
    if (!j.contains("command") || !j["command"].is_string()) {
      throw std::runtime_error(
          "Invalid CommandMessage JSON: missing or invalid 'command' field.");
    }
    if (!j.contains("params") || !j["params"].is_string()) {

      throw std::runtime_error(
          "Invalid CommandMessage JSON: missing or invalid 'params' field.");
    }
    if (!j.contains("sender") || !j["sender"].is_string()) {
      throw std::runtime_error(
          "Invalid CommandMessage JSON: missing or invalid 'sender' field.");
    }

    auto msg = std::make_unique<CommandMessage>(j["command"].get<std::string>(),
                                                j["params"].get<std::string>(),
                                                j["sender"].get<std::string>());

    if (j.contains("timestamp") && j["timestamp"].is_string()) {
      try {
        std::string timestamp_str = j["timestamp"].get<std::string>();
        msg->set_timestamp(tp_utils::string_to_tp(timestamp_str));
      } catch (const std::exception &e) {
        std::cerr
            << "[WARN] CommandMessage::from_json: Failed to parse timestamp: "
            << e.what() << ". Using creation time." << std::endl;
      }
    }
    return msg;
  }

  static std::unique_ptr<CommandMessage>
  create_from_content(const std::string &command, const std::string &params,
                      const std::string &sender) {
    return std::make_unique<CommandMessage>(command, params, sender);
  }

private:
  std::string command_;
  std::string params_;
};