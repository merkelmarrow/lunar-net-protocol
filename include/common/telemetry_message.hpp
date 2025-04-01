// include/common/telemetry_message.hpp

#pragma once

#include "lumen_header.hpp"    // For enums
#include "message.hpp"         // Base class
#include "timepoint_utils.hpp" // For timestamp conversion

#include <iostream>
#include <map>               // For std::map holding telemetry data
#include <memory>            // For std::unique_ptr
#include <nlohmann/json.hpp> // For JSON handling
#include <stdexcept>         // For std::runtime_error
#include <string>

namespace nm = nlohmann; // JSON namespace alias

/**
 * @class TelemetryMessage
 * @brief Represents a telemetry data message, typically sent from Rover to Base
 * Station.
 *
 * Contains a map of key-value pairs representing sensor readings or other
 * telemetry data. Keys are strings, and values are doubles. Inherits from the
 * Message base class.
 *
 * JSON Structure:
 * {
 * "msg_type": "TelemetryMessage",
 * "readings": {
 * "sensor_A": 12.34,
 * "sensor_B": -5.6,
 * ...
 * },
 * "sender": "SENDER_ID",
 * "timestamp": "YYYY-MM-DDTHH:MM:SS+ZZZZ"
 * }
 */
class TelemetryMessage : public Message {
public:
  /**
   * @brief Constructor for TelemetryMessage.
   * @param readings A map where keys are telemetry item names (string) and
   * values are the readings (double).
   * @param sender The identifier of the telemetry sender (usually the Rover).
   */
  TelemetryMessage(const std::map<std::string, double> &readings,
                   const std::string &sender)
      : Message(sender), readings_(readings) {}

  /**
   * @brief Serializes the TelemetryMessage to a JSON string.
   * The readings map is directly serialized into a JSON object.
   * @return std::string JSON representation.
   */
  std::string serialise() const override {
    std::string formatted_time = tp_utils::tp_to_string(timestamp_);
    nm::json j = {{"msg_type", get_type()}, // "TelemetryMessage"
                  {"readings", readings_},  // Serialize map directly
                  {"sender", sender_},
                  {"timestamp", formatted_time}};
    return j.dump();
  }

  /**
   * @brief Gets the message type identifier string.
   * @return std::string "TelemetryMessage".
   */
  std::string get_type() const override { return message_type(); }

  /**
   * @brief Gets the Lumen protocol priority for telemetry.
   * @return LumenHeader::Priority Typically MEDIUM for telemetry data.
   */
  LumenHeader::Priority get_lumen_priority() const override {
    return LumenHeader::Priority::MEDIUM; // Default priority
  }

  /**
   * @brief Gets the Lumen protocol message type for telemetry.
   * @return LumenHeader::MessageType Typically DATA, as it's application data.
   */
  LumenHeader::MessageType get_lumen_type() const override {
    // Could be DATA or a dedicated TELEMETRY type if added to LumenHeader
    return LumenHeader::MessageType::DATA;
  }

  // --- Getter ---
  /**
   * @brief Gets a constant reference to the map of telemetry readings.
   * @return const std::map<std::string, double>&
   */
  const std::map<std::string, double> &get_readings() const {
    return readings_;
  }

  // --- Static Factory Methods ---

  /**
   * @brief Static method to get the type identifier string.
   * @return std::string "TelemetryMessage".
   */
  static std::string message_type() { return "TelemetryMessage"; }

  /**
   * @brief Static factory method to create a TelemetryMessage from a JSON
   * object.
   * @param j nlohmann::json object representing the telemetry message.
   * @return std::unique_ptr<TelemetryMessage> Pointer to the created object.
   * @throws std::runtime_error if required JSON fields ("readings", "sender")
   * are missing or invalid.
   * @throws nlohmann::json::type_error if "readings" is not a valid JSON object
   * convertible to the map type.
   */
  static std::unique_ptr<TelemetryMessage> from_json(const nm::json &j) {
    // Validate required fields
    if (!j.contains("readings") || !j["readings"].is_object()) {
      throw std::runtime_error("Invalid TelemetryMessage JSON: missing or "
                               "invalid 'readings' field (must be an object).");
    }
    if (!j.contains("sender") || !j["sender"].is_string()) {
      throw std::runtime_error(
          "Invalid TelemetryMessage JSON: missing or invalid 'sender' field.");
    }

    // Create object - nlohmann::json handles map conversion directly
    auto msg = std::make_unique<TelemetryMessage>(
        j["readings"]
            .get<std::map<std::string, double>>(), // Let json library handle
                                                   // map conversion
        j["sender"].get<std::string>());

    // Parse timestamp if present
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

  /**
   * @brief Static factory method to create a TelemetryMessage directly from a
   * readings map. Primarily used by `Message::create`.
   * @param readings The map containing telemetry readings.
   * @param sender The sender ID string.
   * @return std::unique_ptr<TelemetryMessage> Pointer to the created object.
   */
  static std::unique_ptr<TelemetryMessage>
  create_from_content(const std::map<std::string, double> &readings,
                      const std::string &sender) {
    return std::make_unique<TelemetryMessage>(readings, sender);
  }

private:
  std::map<std::string, double>
      readings_; ///< Map holding telemetry data points.
};