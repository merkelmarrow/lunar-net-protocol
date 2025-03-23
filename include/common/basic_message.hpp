// include/common/basic_message.hpp

#pragma once

#include "message.hpp"
#include "timepoint_utils.hpp"

#include <string>

#include <nlohmann/json.hpp>

namespace {
namespace nm = nlohmann;
}

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

  // Static methods needed for factory
  static std::string message_type() { return "BasicMessage"; }

  static std::unique_ptr<BasicMessage> from_json(const nm::json &j) {
    if (!j.contains("content") || !j.contains("sender")) {
      throw std::runtime_error(
          "Invalid text message JSON: missing required fields");
    }

    auto msg = std::make_unique<BasicMessage>(j["content"].get<std::string>(),
                                              j["sender"].get<std::string>());

    if (j.contains("timestamp") && j["timestamp"].is_string()) {
      std::string timestamp_str = j["timestamp"].get<std::string>();
      std::chrono::system_clock::time_point parsed_time =
          tp_utils::string_to_tp(timestamp_str);
      msg->set_timestamp(parsed_time);
    }

    return msg;
  }

  static std::unique_ptr<BasicMessage>
  create_from_content(const std::string &content, const std::string &sender) {
    return std::make_unique<BasicMessage>(content, sender);
  }

  // Getter
  const std::string &get_content() const { return content_; }

private:
  std::string content_;
};