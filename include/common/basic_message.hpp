// include/common/basic_message.hpp

#pragma once

#include "lumen_header.hpp"    // For LumenHeader enums
#include "message.hpp"         // Include base class definition
#include "timepoint_utils.hpp" // For timestamp conversion utilities

#include <iostream>
#include <memory>            // For std::unique_ptr
#include <nlohmann/json.hpp> // For JSON handling
#include <stdexcept>         // For std::runtime_error
#include <string>

namespace nm = nlohmann; // Alias for nlohmann::json namespace

/**
 * @class BasicMessage
 * @brief Represents a simple text-based message for general communication or
 * testing.
 *
 * Inherits from the Message base class and provides functionality to serialize
 * and deserialize a message containing a single string content field.
 */
class BasicMessage : public Message {
public:
  /**
   * @brief Constructor for BasicMessage.
   * @param content The string content of the message.
   * @param sender The identifier of the message sender.
   */
  BasicMessage(const std::string &content, const std::string &sender)
      : Message(sender), content_(content) {}

  /**
   * @brief Serializes the BasicMessage object into a JSON string.
   * Includes "msg_type", "content", "sender", and "timestamp" fields.
   * @return std::string JSON representation of the message.
   */
  std::string serialise() const override {
    // Format timestamp using utility function
    std::string formatted_time = tp_utils::tp_to_string(timestamp_);
    nm::json j = {
        {"msg_type", get_type()}, // Use virtual get_type() -> message_type()
        {"content", content_},
        {"sender", sender_},
        {"timestamp", formatted_time}};
    return j.dump(); // Convert JSON object to string
  }

  /**
   * @brief Returns the specific type identifier for this message class.
   * @return std::string "BasicMessage".
   */
  std::string get_type() const override { return message_type(); }

  /**
   * @brief Returns the default Lumen protocol priority for this message type.
   * @return LumenHeader::Priority Typically MEDIUM for basic data messages.
   */
  LumenHeader::Priority get_lumen_priority() const override {
    return LumenHeader::Priority::MEDIUM; // Default priority
  }

  /**
   * @brief Returns the default Lumen protocol message type for this message.
   * @return LumenHeader::MessageType Typically DATA for general messages.
   */
  LumenHeader::MessageType get_lumen_type() const override {
    return LumenHeader::MessageType::DATA; // Represents generic data transfer
  }

  /**
   * @brief Gets the string content of the message.
   * @return const std::string& Reference to the content string.
   */
  const std::string &get_content() const { return content_; }

  // --- Static Factory Methods ---

  /**
   * @brief Static method to get the type identifier string for BasicMessage.
   * Used by the Message factory (`Message::deserialise`).
   * @return std::string "BasicMessage".
   */
  static std::string message_type() { return "BasicMessage"; }

  /**
   * @brief Static factory method to create a BasicMessage object from a JSON
   * object. Called by `Message::deserialise` when "msg_type" is "BasicMessage".
   * @param j The nlohmann::json object representing the message.
   * @return std::unique_ptr<BasicMessage> Pointer to the created BasicMessage
   * object.
   * @throws std::runtime_error if required fields ("content", "sender") are
   * missing or invalid in the JSON.
   */
  static std::unique_ptr<BasicMessage> from_json(const nm::json &j) {
    // Validate required fields exist
    if (!j.contains("content") || !j["content"].is_string()) {
      throw std::runtime_error(
          "Invalid BasicMessage JSON: missing or invalid 'content' field.");
    }
    if (!j.contains("sender") || !j["sender"].is_string()) {
      // This check might be redundant if Message::deserialise already checks
      // sender
      throw std::runtime_error(
          "Invalid BasicMessage JSON: missing or invalid 'sender' field.");
    }

    // Create the message object using validated fields
    auto msg = std::make_unique<BasicMessage>(j["content"].get<std::string>(),
                                              j["sender"].get<std::string>());

    // Parse timestamp if present
    if (j.contains("timestamp") && j["timestamp"].is_string()) {
      try {
        std::string timestamp_str = j["timestamp"].get<std::string>();
        std::chrono::system_clock::time_point parsed_time =
            tp_utils::string_to_tp(timestamp_str);
        msg->set_timestamp(parsed_time); // Use protected setter from base class
      } catch (const std::exception &e) {
        // Log warning or rethrow if timestamp parsing failure is critical
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

  /**
   * @brief Static factory method to create a BasicMessage object directly from
   * content string. Primarily used by `Message::create`.
   * @param content The string content for the message.
   * @param sender The sender ID string.
   * @return std::unique_ptr<BasicMessage> Pointer to the created BasicMessage
   * object.
   */
  static std::unique_ptr<BasicMessage>
  create_from_content(const std::string &content, const std::string &sender) {
    return std::make_unique<BasicMessage>(content, sender);
  }

private:
  std::string content_; ///< The main string content of the message.
};