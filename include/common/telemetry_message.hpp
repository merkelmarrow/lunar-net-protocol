// include/common/telemetry_message.hpp

#pragma once

#include "lumen_header.hpp"
#include "message.hpp"
#include "timepoint_utils.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace nm = nlohmann;

// represents a telemetry data message, typically sent from rover to base
// station
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
    return LumenHeader::Priority::MEDIUM; // default priority
  }

  LumenHeader::MessageType get_lumen_type() const override {
    return LumenHeader::MessageType::DATA;
  }

  // --- getter ---
  const std::map<std::string, double> &get_readings() const {
    return readings_;
  }

  // --- static factory methods ---

  static std::string message_type() { return "TelemetryMessage"; }

  static std::unique_ptr<TelemetryMessage> from_json(const nm::json &j) {
    if (!j.contains("readings") || !j["readings"].is_object()) {
      throw std::runtime_error("Invalid TelemetryMessage JSON: missing or "
                               "invalid 'readings' field (must be an object).");
    }
    if (!j.contains("sender") || !j["sender"].is_string()) {
      throw std::runtime_error(
          "Invalid TelemetryMessage JSON: missing or invalid 'sender' field.");
    }

    auto msg = std::make_unique<TelemetryMessage>(
        j["readings"].get<std::map<std::string, double>>(),
        j["sender"].get<std::string>());

    if (j.contains("timestamp") && j["timestamp"].is_string()) {
      try {
        std::string timestamp_str = j["timestamp"].get<std::string>();
        msg->set_timestamp(tp_utils::string_to_tp(timestamp_str));
      } catch (const std::exception &e) {
        std::cerr
            << "[WARN] TelemetryMessage::from_json: Failed to parse timestamp: "
            << e.what() << ". Using creation time." << std::endl;
      }
    }

    return msg;
  }

  static std::unique_ptr<TelemetryMessage>
  create_from_content(const std::map<std::string, double> &readings,
                      const std::string &sender) {
    return std::make_unique<TelemetryMessage>(readings, sender);
  }

private:
  std::map<std::string, double> readings_;
};