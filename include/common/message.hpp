// inlude/common/message.hpp

#pragma once

#include "nlohmann/json.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace {
namespace nm = nlohmann;
}

// define a factory function type
class Message;
using MessageFactory =
    std::function<std::unique_ptr<Message>(const nm::json &)>;

// base class for all message types
class Message {
public:
  virtual ~Message() = default;

  // serialise the message to a json string
  virtual std::string serialise() const = 0;

  // get message type
  virtual std::string get_type() const = 0;

  const std::string &get_sender() const { return sender_; }
  std::chrono::system_clock::time_point get_timestamp() const {
    return timestamp_;
  }

  // utility functions
  static bool is_valid_json(const std::string &json_str);
  static std::string pretty_print(const std::string &json_str);

  // factory method to create a message of the specified type
  static std::unique_ptr<Message> create(const std::string &msg_type,
                                         const std::string &content,
                                         const std::string &sender);

  // create a message object from JSON
  static std::unique_ptr<Message> deserialise(const std::string &json_str);

  // register a message with its factory function
  static void register_message_type(const std::string &msg_type,
                                    MessageFactory factory);

protected:
  Message(const std::string &sender)
      : sender_(sender), timestamp_(std::chrono::system_clock::now()) {}

  // fields for all messages
  std::string sender_;
  std::chrono::system_clock::time_point timestamp_;

private:
  // registry of message types
  static std::unordered_map<std::string, MessageFactory> &
  get_message_registry();
};

// template to help with message registrations
template <typename T> class RegisteredMessage : public Message {
public:
  RegisteredMessage(const std::string &sender) : Message(sender) {
    // This constructor is protected to prevent direct instantiation
  }

  /**
   * Automatically register this message type
   */
  static bool register_type() {
    Message::register_message_type(
        T::message_type(), [](const nm::json &j) -> std::unique_ptr<Message> {
          return T::from_json(j);
        });
    return true;
  }
};