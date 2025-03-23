// inlude/common/message.hpp

#pragma once

#include <chrono>
#include <memory>
#include <string>

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

  // create a message object from JSON string
  static std::unique_ptr<Message> deserialise(const std::string &json_str);

  // factory method to create a message of the specified type
  static std::unique_ptr<Message> create(const std::string &type,
                                         const std::string &content,
                                         const std::string &sender);

protected:
  Message(const std::string &sender)
      : sender_(sender), timestamp_(std::chrono::system_clock::now()) {}

  // fields for all messages
  std::string sender_;
  std::chrono::system_clock::time_point timestamp_;
};