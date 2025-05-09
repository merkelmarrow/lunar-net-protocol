// include/common/basic_message.hpp

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

// represents a simple text-based message
class BasicMessage : public Message {
public:
  BasicMessage(const std::string &content, const std::string &sender)
      : Message(sender), content_(content) {}

  std::string serialise() const override {
    std::string formatted_time = tp_utils::tp_to_string(timestamp_);
    nm::json j = {{"msg_type", get_type()},
                  {"content", content_},
                  {"sender", sender_},
                  {"timestamp", formatted_time}};
    return j.dump();
  }

  std::string get_type() const override { return message_type(); }

  LumenHeader::Priority get_lumen_priority() const override {
    return LumenHeader::Priority::MEDIUM;
  }

  LumenHeader::MessageType get_lumen_type() const override {
    return LumenHeader::MessageType::DATA;
  }

  const std::string &get_content() const { return content_; }

  // --- static factory methods ---

  static std::string message_type() { return "BasicMessage"; }

  static std::unique_ptr<BasicMessage> from_json(const nm::json &j) {
    if (!j.contains("content") || !j["content"].is_string()) {
      throw std::runtime_error(
          "Invalid BasicMessage JSON: missing or invalid 'content' field.");
    }
    if (!j.contains("sender") || !j["sender"].is_string()) {

      throw std::runtime_error(
          "Invalid BasicMessage JSON: missing or invalid 'sender' field.");
    }

    auto msg = std::make_unique<BasicMessage>(j["content"].get<std::string>(),
                                              j["sender"].get<std::string>());

    if (j.contains("timestamp") && j["timestamp"].is_string()) {
      try {
        std::string timestamp_str = j["timestamp"].get<std::string>();
        std::chrono::system_clock::time_point parsed_time =
            tp_utils::string_to_tp(timestamp_str);
        msg->set_timestamp(parsed_time);
      } catch (const std::exception &e) {

        std::cerr << "[WARN] BasicMessage::from_json: Failed to parse "
                     "timestamp string '"
                  << (j.contains("timestamp")
                          ? j["timestamp"].get<std::string>()
                          : "N/A")
                  << "'. Error: " << e.what() << ". Using creation time."
                  << std::endl;
      }
    }

    return msg;
  }

  static std::unique_ptr<BasicMessage>
  create_from_content(const std::string &content, const std::string &sender) {
    return std::make_unique<BasicMessage>(content, sender);
  }

private:
  std::string content_;
};