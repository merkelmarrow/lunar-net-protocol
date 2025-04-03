// include/common/status_message.hpp

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

// represents a status update message, sent from rover to base station
class StatusMessage : public Message {
public:
  // defines the severity level of the status report
  enum class StatusLevel {
    OK,
    WARNING,
    ERROR,
    CRITICAL // non-recoverable error or critical failure
  };

  StatusMessage(StatusLevel level, const std::string &description,
                const std::string &sender)
      : Message(sender), level_(level), description_(description) {}

  std::string serialise() const override {
    std::string formatted_time = tp_utils::tp_to_string(timestamp_);

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
    default:
      level_str = "UNKNOWN";
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

  // gets the lumen protocol priority based on the status level
  LumenHeader::Priority get_lumen_priority() const override {
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
    return LumenHeader::MessageType::STATUS;
  }

  // --- getters ---
  StatusLevel get_level() const { return level_; }
  const std::string &get_description() const { return description_; }

  // --- static factory methods ---

  static std::string message_type() { return "StatusMessage"; }

  static std::unique_ptr<StatusMessage> from_json(const nm::json &j) {
    if (!j.contains("level") || !j["level"].is_string()) {
      throw std::runtime_error(
          "Invalid StatusMessage JSON: missing or invalid 'level' field.");
    }
    if (!j.contains("description") || !j["description"].is_string()) {
      throw std::runtime_error("Invalid StatusMessage JSON: missing or invalid "
                               "'description' field.");
    }
    if (!j.contains("sender") || !j["sender"].is_string()) {
      throw std::runtime_error(
          "Invalid StatusMessage JSON: missing or invalid 'sender' field.");
    }

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
      throw std::runtime_error("Invalid status level string in JSON: " +
                               level_str);

    auto msg = std::make_unique<StatusMessage>(
        level, j["description"].get<std::string>(),
        j["sender"].get<std::string>());

    if (j.contains("timestamp") && j["timestamp"].is_string()) {
      try {
        std::string timestamp_str = j["timestamp"].get<std::string>();
        msg->set_timestamp(tp_utils::string_to_tp(timestamp_str));
      } catch (const std::exception &e) {
        std::cerr
            << "[WARN] StatusMessage::from_json: Failed to parse timestamp: "
            << e.what() << ". Using creation time." << std::endl;
      }
    }

    return msg;
  }

  static std::unique_ptr<StatusMessage>
  create_from_content(StatusLevel level, const std::string &description,
                      const std::string &sender) {
    return std::make_unique<StatusMessage>(level, description, sender);
  }

private:
  StatusLevel level_;
  std::string description_;
};