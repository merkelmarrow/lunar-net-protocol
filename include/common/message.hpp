// include/common/message.hpp

#pragma once

#include "lumen_header.hpp" // For LumenHeader enums used by derived classes
#include <chrono>           // For std::chrono::system_clock::time_point
#include <memory>           // For std::unique_ptr
#include <string>

/**
 * @class Message
 * @brief Abstract base class for all application-level message types.
 *
 * Defines the common interface and properties for messages exchanged between
 * the Base Station and Rover applications. Subclasses must implement methods
 * for serialization, type identification, and mapping to LUMEN protocol
 * parameters.
 *
 * This class also provides static factory methods (`deserialise`, `create`) for
 * creating specific message subclass instances from JSON strings or basic
 * content. The `deserialise` method uses an X-macro pattern with
 * `message_types.hpp` to manage the factory logic for different message types.
 */
class Message {
public:
  /**
   * @brief Virtual destructor to ensure proper cleanup of derived classes.
   */
  virtual ~Message() = default;

  /**
   * @brief Pure virtual method to serialize the message content into a JSON
   * string. Subclasses must implement this to define their JSON representation.
   * @return std::string JSON representation of the message.
   */
  virtual std::string serialise() const = 0;

  /**
   * @brief Pure virtual method to get the specific type identifier string for
   * the message subclass. Used by the factory method (`deserialise`) to
   * identify the correct subclass. Subclasses typically return the result of
   * their static `message_type()` method.
   * @return std::string The message type identifier (e.g., "BasicMessage",
   * "CommandMessage").
   */
  virtual std::string get_type() const = 0;

  /**
   * @brief Gets the identifier of the message sender.
   * @return const std::string& Reference to the sender ID string.
   */
  const std::string &get_sender() const { return sender_; }

  /**
   * @brief Gets the timestamp when the message object was created (or set
   * during deserialization).
   * @return std::chrono::system_clock::time_point The timestamp.
   */
  std::chrono::system_clock::time_point get_timestamp() const {
    return timestamp_;
  }

  /**
   * @brief Pure virtual method to get the appropriate Lumen protocol priority
   * for this message type.
   * @return LumenHeader::Priority The corresponding priority level.
   */
  virtual LumenHeader::Priority get_lumen_priority() const = 0;

  /**
   * @brief Pure virtual method to get the appropriate Lumen protocol message
   * type for this message.
   * @return LumenHeader::MessageType The corresponding Lumen message type.
   */
  virtual LumenHeader::MessageType get_lumen_type() const = 0;

  // --- Static Utility & Factory Methods ---

  /**
   * @brief Utility function to check if a given string is structurally valid
   * JSON.
   * @param json_str The string to check.
   * @return True if the string can be parsed as JSON, false otherwise.
   */
  static bool is_valid_json(const std::string &json_str);

  /**
   * @brief Utility function to format a JSON string with indentation for better
   * readability.
   * @param json_str A valid JSON string.
   * @return std::string The formatted JSON string with 4-space indentation.
   * @throws std::runtime_error if the input string is not valid JSON.
   */
  static std::string pretty_print(const std::string &json_str);

  /**
   * @brief Factory method to deserialize a JSON string into a specific Message
   * subclass object. Parses the JSON, identifies the message type using the
   * "msg_type" field, and calls the corresponding subclass's static `from_json`
   * method. Uses the MESSAGE_TYPES_LIST macro.
   * @param json_str The JSON string representation of the message.
   * @return std::unique_ptr<Message> A pointer to the created Message subclass
   * instance.
   * @throws std::runtime_error if parsing fails, "msg_type" is missing/invalid,
   * the type is unknown, or if the subclass's `from_json` method throws an
   * error (e.g., missing required fields).
   */
  static std::unique_ptr<Message> deserialise(const std::string &json_str);

protected:
  /**
   * @brief Protected constructor for base class initialization by derived
   * classes. Sets the sender ID and initializes the timestamp to the current
   * time.
   * @param sender The identifier of the message sender.
   */
  Message(const std::string &sender)
      : sender_(sender), timestamp_(std::chrono::system_clock::now()) {}

  /**
   * @brief Protected method allowing derived classes (specifically their
   * `from_json` methods) to set the timestamp based on the value parsed from
   * the JSON, overriding the default creation time.
   * @param timestamp The timestamp parsed from the message data.
   */
  void set_timestamp(const std::chrono::system_clock::time_point &timestamp) {
    timestamp_ = timestamp;
  };

  // --- Common Member Variables ---
  std::string sender_; ///< Identifier of the node that sent the message.
  std::chrono::system_clock::time_point
      timestamp_; ///< Time the message was created or timestamp from received
                  ///< data.
};