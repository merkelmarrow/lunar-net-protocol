// include/common/command_message.hpp

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
 * @class CommandMessage
 * @brief Represents a command sent between nodes (e.g., Base Station to Rover).
 *
 * Contains a command string and an optional parameters string.
 * Inherits from the Message base class.
 *
 * JSON Structure:
 * {
 * "msg_type": "CommandMessage",
 * "command": "COMMAND_STRING",
 * "params": "PARAMETERS_STRING",
 * "sender": "SENDER_ID",
 * "timestamp": "YYYY-MM-DDTHH:MM:SS+ZZZZ"
 * }
 */
class CommandMessage : public Message {
public:
  /**
   * @brief Constructor for CommandMessage.
   * @param command The command identifier string (e.g., "MOVE_FORWARD",
   * "SESSION_INIT").
   * @param params A string containing any necessary parameters for the command.
   * @param sender The identifier of the command sender.
   */
  CommandMessage(const std::string &command, const std::string &params,
                 const std::string &sender)
      : Message(sender), command_(command), params_(params) {}

  /**
   * @brief Serializes the CommandMessage to a JSON string.
   * @return std::string JSON representation.
   */
  std::string serialise() const override {
    std::string formatted_time = tp_utils::tp_to_string(timestamp_);
    nm::json j = {{"msg_type", get_type()}, // "CommandMessage"
                  {"command", command_},
                  {"params", params_},
                  {"sender", sender_},
                  {"timestamp", formatted_time}};
    return j.dump();
  }

  /**
   * @brief Gets the message type identifier string.
   * @return std::string "CommandMessage".
   */
  std::string get_type() const override { return message_type(); }

  /**
   * @brief Gets the Lumen protocol priority for commands.
   * @return LumenHeader::Priority Typically HIGH for commands.
   */
  LumenHeader::Priority get_lumen_priority() const override {
    return LumenHeader::Priority::HIGH; // Commands are high priority
  }

  /**
   * @brief Gets the Lumen protocol message type for commands.
   * @return LumenHeader::MessageType CMD.
   */
  LumenHeader::MessageType get_lumen_type() const override {
    return LumenHeader::MessageType::CMD; // Maps to command type in protocol
  }

  // --- Getters ---
  /**
   * @brief Gets the command string.
   * @return const std::string&
   */
  const std::string &get_command() const { return command_; }

  /**
   * @brief Gets the parameters string.
   * @return const std::string&
   */
  const std::string &get_params() const { return params_; }

  // --- Static Factory Methods ---

  /**
   * @brief Static method to get the type identifier string.
   * @return std::string "CommandMessage".
   */
  static std::string message_type() { return "CommandMessage"; }

  /**
   * @brief Static factory method to create a CommandMessage from a JSON object.
   * @param j nlohmann::json object representing the command message.
   * @return std::unique_ptr<CommandMessage> Pointer to the created object.
   * @throws std::runtime_error if required JSON fields ("command", "params",
   * "sender") are missing or invalid.
   */
  static std::unique_ptr<CommandMessage> from_json(const nm::json &j) {
    // Validate required fields
    if (!j.contains("command") || !j["command"].is_string()) {
      throw std::runtime_error(
          "Invalid CommandMessage JSON: missing or invalid 'command' field.");
    }
    if (!j.contains("params") || !j["params"].is_string()) {
      // Allow params to potentially be non-string in future? For now, require
      // string.
      throw std::runtime_error(
          "Invalid CommandMessage JSON: missing or invalid 'params' field.");
    }
    if (!j.contains("sender") || !j["sender"].is_string()) {
      throw std::runtime_error(
          "Invalid CommandMessage JSON: missing or invalid 'sender' field.");
    }

    // Create object
    auto msg = std::make_unique<CommandMessage>(
        j["command"].get<std::string>(),
        j["params"]
            .get<std::string>(), // Assuming params is always a string in JSON
        j["sender"].get<std::string>());

    // Parse timestamp if present
    if (j.contains("timestamp") && j["timestamp"].is_string()) {
      try {
        std::string timestamp_str = j["timestamp"].get<std::string>();
        msg->set_timestamp(tp_utils::string_to_tp(timestamp_str));
      } catch (const std::exception &e) {
        std::cerr
            << "[WARN] CommandMessage::from_json: Failed to parse timestamp: "
            << e.what() << ". Using creation time." << std::endl;
      }
    }
    return msg;
  }

  /**
   * @brief Static factory method to create a CommandMessage directly from
   * command/params strings. Primarily used by `Message::create`.
   * @param command The command string.
   * @param params The parameters string.
   * @param sender The sender ID string.
   * @return std::unique_ptr<CommandMessage> Pointer to the created object.
   */
  static std::unique_ptr<CommandMessage>
  create_from_content(const std::string &command, const std::string &params,
                      const std::string &sender) {
    return std::make_unique<CommandMessage>(command, params, sender);
  }

private:
  std::string command_; ///< The command identifier string.
  std::string params_;  ///< String containing parameters for the command.
};