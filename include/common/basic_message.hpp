#pragma once

#include "message.hpp"
#include <string>

#include <format>

#include <nlohmann/json.hpp>

namespace {
namespace nm = nlohmann;
}

class BasicMessage : public Message {
public:
  BasicMessage(const std::string &content, const std::string &sender)
      : Message(sender), content_(content) {}

  std::string serialise() const override {
    std::string formatted_time = std::format("{:%FT%T%z}", timestamp_);
    nm::json j = {{"type", get_type()},
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

    return std::make_unique<BasicMessage>(j["content"].get<std::string>(),
                                          j["sender"].get<std::string>());
  }

private:
  std::string content_;
};