// include/common/status_message.hpp

#pragma once

#include "lumen_header.hpp"    // For enums
#include "message.hpp"         // Base class
#include "timepoint_utils.hpp" // For timestamp conversion

#include <iostream>
#include <memory>            // For std::unique_ptr
#include <nlohmann/json.hpp> // For JSON handling
#include <stdexcept>         // For std::runtime_error
#include <string>

namespace nm = nlohmann; // JSON namespace alias

/**
 * @class StatusMessage
 * @brief Represents a status update message, typically sent from Rover to Base
 * Station.
 *
 * Contains a status level (OK, WARNING, ERROR, CRITICAL) and a descriptive
 * string. Inherits from the Message base class.
 *
 * JSON Structure:
 * {
 * "msg_type": "StatusMessage",
 * "level": "OK" | "WARNING" | "ERROR" | "CRITICAL",
 * "description": "STATUS_DESCRIPTION_STRING",
 * "sender": "SENDER_ID",
 * "timestamp": "YYYY-MM-DDTHH:MM:SS+ZZZZ"
 * }
 */
class StatusMessage : public Message {
public:
  /**
   * @enum StatusLevel
   * @brief Defines the severity level of the status report.
   */
  enum class StatusLevel {
    OK,      ///< Nominal operation.
    WARNING, ///< Potential issue detected.
    ERROR,   ///< Recoverable error occurred.
    CRITICAL ///< Non-recoverable error or critical failure.
  };

  /**
   * @brief Constructor for StatusMessage.
   * @param level The StatusLevel enum value.
   * @param description A string describing the status.
   * @param sender The identifier of the status sender (usually the Rover).
   */
  StatusMessage(StatusLevel level, const std::string &description,
                const std::string &sender)
      : Message(sender), level_(level), description_(description) {}

  /**
   * @brief Serializes the StatusMessage to a JSON string.
   * Converts the StatusLevel enum to its string representation.
   * @return std::string JSON representation.
   */
  std::string serialise() const override {
    std::string formatted_time = tp_utils::tp_to_string(timestamp_);

    // Convert StatusLevel enum to string for JSON
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
      break; // Should not happen
    }

    nm::json j = {{"msg_type", get_type()}, // "StatusMessage"
                  {"level", level_str},
                  {"description", description_},
                  {"sender", sender_},
                  {"timestamp", formatted_time}};
    return j.dump();
  }

  /**
   * @brief Gets the message type identifier string.
   * @return std::string "StatusMessage".
   */
  std::string get_type() const override { return message_type(); }

  /**
   * @brief Gets the Lumen protocol priority based on the status level.
   * @return LumenHeader::Priority Higher priority for more severe statuses.
   */
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

  /**
   * @brief Gets the Lumen protocol message type for status updates.
   * @return LumenHeader::MessageType STATUS.
   */
  LumenHeader::MessageType get_lumen_type() const override {
    return LumenHeader::MessageType::STATUS;
  }

  // --- Getters ---
  /**
   * @brief Gets the status level enum value.
   * @return StatusLevel
   */
  StatusLevel get_level() const { return level_; }

  /**
   * @brief Gets the status description string.
   * @return const std::string&
   */
  const std::string &get_description() const { return description_; }

  // --- Static Factory Methods ---

  /**
   * @brief Static method to get the type identifier string.
   * @return std::string "StatusMessage".
   */
  static std::string message_type() { return "StatusMessage"; }

  /**
   * @brief Static factory method to create a StatusMessage from a JSON object.
   * Parses the "level" string back into the StatusLevel enum.
   * @param j nlohmann::json object representing the status message.
   * @return std::unique_ptr<StatusMessage> Pointer to the created object.
   * @throws std::runtime_error if required JSON fields ("level", "description",
   * "sender") are missing/invalid, or if the "level" string is not recognized.
   */
  static std::unique_ptr<StatusMessage> from_json(const nm::json &j) {
    // Validate required fields
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

    // Convert level string back to enum
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

    // Create object
    auto msg = std::make_unique<StatusMessage>(
        level, j["description"].get<std::string>(),
        j["sender"].get<std::string>());

    // Parse timestamp if present
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

  /**
   * @brief Static factory method to create a StatusMessage directly from level
   * and description. Primarily used by `Message::create`.
   * @param level The StatusLevel enum value.
   * @param description The status description string.
   * @param sender The sender ID string.
   * @return std::unique_ptr<StatusMessage> Pointer to the created object.
   */
  static std::unique_ptr<StatusMessage>
  create_from_content(StatusLevel level, const std::string &description,
                      const std::string &sender) {
    return std::make_unique<StatusMessage>(level, description, sender);
  }

private:
  StatusLevel level_;       ///< The severity level of the status.
  std::string description_; ///< Text description accompanying the status level.
};