// include/common/status_message.hpp

#pragma once

#include "message.hpp"
#include "timepoint_utils.hpp"

#include <string>

#include <nlohmann/json.hpp>

namespace nm = nlohmann;

class StatusMessage : public Message {
public:
  enum class StatusLevel { OK, WARNING, ERROR, CRITICAL };

  StatusMessage(StatusLevel level, const std::string &description,
                const std::string &sender)
      : Message(sender), level_(level), description_(description) {}

  std::string serialise() const override {
    std::string formatted_time = tp_utils::tp_to_string(timestamp_);

    // convert enum to string
    std::string level_str;
    switch (level_) {
    case StatusLevel::OK:
      level_str = "OK";
      break;
    case StatusLevel::WARNING:
      level_str = "WARNING";
      break;
    case StatusLevel::ERROR:
      level_str = "ERROR";
      break;
    case StatusLevel::CRITICAL:
      level_str = "CRITICAL";
      break;
    }

    nm::json j = {{"msg_type", get_type()},
                  {"level", level_str},
                  {"description", description_},
                  {"sender", sender_},
                  {"timestamp", formatted_time}};
    return j.dump();
  }

  std::string get_type() const override { return message_type(); }

  LumenHeader::Priority get_lumen_priority() const override {
    // priority based on status level
    switch (level_) {
    case StatusLevel::OK:
      return LumenHeader::Priority::LOW;
    case StatusLevel::WARNING:
      return LumenHeader::Priority::MEDIUM;
    case StatusLevel::ERROR:
    case StatusLevel::CRITICAL:
      return LumenHeader::Priority::HIGH;
    default:
      return LumenHeader::Priority::MEDIUM;
    }
  }

  LumenHeader::MessageType get_lumen_type() const override {
    return LumenHeader::MessageType::STATUS; // status message type
  }

  // static methods needed for factory
  static std::string message_type() { return "StatusMessage"; }

  static std::unique_ptr<StatusMessage> from_json(const nm::json &j) {
    if (!j.contains("level") || !j.contains("description") ||
        !j.contains("sender")) {
      throw std::runtime_error(
          "Invalid status message JSON: missing required fields");
    }

    // convert string to enum
    std::string level_str = j["level"].get<std::string>();
    StatusLevel level;
    if (level_str == "OK")
      level = StatusLevel::OK;
    else if (level_str == "WARNING")
      level = StatusLevel::WARNING;
    else if (level_str == "ERROR")
      level = StatusLevel::ERROR;
    else if (level_str == "CRITICAL")
      level = StatusLevel::CRITICAL;
    else
      throw std::runtime_error("Invalid status level: " + level_str);

    auto msg = std::make_unique<StatusMessage>(
        level, j["description"].get<std::string>(),
        j["sender"].get<std::string>());

    if (j.contains("timestamp") && j["timestamp"].is_string()) {
      std::string timestamp_str = j["timestamp"].get<std::string>();
      std::chrono::system_clock::time_point parsed_time =
          tp_utils::string_to_tp(timestamp_str);
      msg->set_timestamp(parsed_time);
    }

    return msg;
  }

  static std::unique_ptr<StatusMessage>
  create_from_content(StatusLevel level, const std::string &description,
                      const std::string &sender) {
    return std::make_unique<StatusMessage>(level, description, sender);
  }

  // getters
  StatusLevel get_level() const { return level_; }
  const std::string &get_description() const { return description_; }

private:
  StatusLevel level_;
  std::string description_;
};