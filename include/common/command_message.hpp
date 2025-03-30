// include/common/command_message.hpp

#pragma once

#include "message.hpp"
#include "timepoint_utils.hpp"

#include <string>

#include <nlohmann/json.hpp>

namespace {
namespace nm = nlohmann;
}

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
    return LumenHeader::MessageType::CMD;
  }

  // Static methods needed for factory
  static std::string message_type() { return "CommandMessage"; }

  static std::unique_ptr<CommandMessage> from_json(const nm::json &j) {
    if (!j.contains("command") || !j.contains("params") ||
        !j.contains("sender")) {
      throw std::runtime_error(
          "Invalid command message JSON: missing required fields");
    }

    auto msg = std::make_unique<CommandMessage>(j["command"].get<std::string>(),
                                                j["params"].get<std::string>(),
                                                j["sender"].get<std::string>());

    if (j.contains("timestamp") && j["timestamp"].is_string()) {
      std::string timestamp_str = j["timestamp"].get<std::string>();
      std::chrono::system_clock::time_point parsed_time =
          tp_utils::string_to_tp(timestamp_str);
      msg->set_timestamp(parsed_time);
    }

    return msg;
  }

  static std::unique_ptr<CommandMessage>
  create_from_content(const std::string &command, const std::string &params,
                      const std::string &sender) {
    return std::make_unique<CommandMessage>(command, params, sender);
  }

  const std::string &get_command() const { return command_; }
  const std::string &get_params() const { return params_; }

private:
  std::string command_;
  std::string params_;
};