// include/common/telemetry_message.hpp

#pragma once

#include "message.hpp"
#include "timepoint_utils.hpp"

#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace {
namespace nm = nlohmann;
}

class TelemetryMessage : public Message {
public:
  TelemetryMessage(const std::map<std::string, double> &readings,
                   const std::string &sender)
      : Message(sender), readings_(readings) {}

  std::string serialise() const override {
    std::string formatted_time = tp_utils::tp_to_string(timestamp_);
    nm::json j = {{"msg_type", get_type()},
                  {"readings", readings_},
                  {"sender", sender_},
                  {"timestamp", formatted_time}};
    return j.dump();
  }

  std::string get_type() const override { return message_type(); }

  LumenHeader::Priority get_lumen_priority() const override {
    return LumenHeader::Priority::MEDIUM; // default
  }

  LumenHeader::MessageType get_lumen_type() const override {
    return LumenHeader::MessageType::DATA;
  }

  // static methods needed for factory
  static std::string message_type() { return "TelemetryMessage"; }

  static std::unique_ptr<TelemetryMessage> from_json(const nm::json &j) {
    if (!j.contains("readings") || !j.contains("sender")) {
      throw std::runtime_error(
          "Invalid telemetry message JSON: missing required fields");
    }

    auto msg = std::make_unique<TelemetryMessage>(
        j["readings"].get<std::map<std::string, double>>(),
        j["sender"].get<std::string>());

    if (j.contains("timestamp") && j["timestamp"].is_string()) {
      std::string timestamp_str = j["timestamp"].get<std::string>();
      std::chrono::system_clock::time_point parsed_time =
          tp_utils::string_to_tp(timestamp_str);
      msg->set_timestamp(parsed_time);
    }

    return msg;
  }

  static std::unique_ptr<TelemetryMessage>
  create_from_content(const std::map<std::string, double> &readings,
                      const std::string &sender) {
    return std::make_unique<TelemetryMessage>(readings, sender);
  }

  // Getter
  const std::map<std::string, double> &get_readings() const {
    return readings_;
  }

private:
  std::map<std::string, double> readings_;
};