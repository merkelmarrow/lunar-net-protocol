#pragma once

#include <chrono>
#include <memory>
#include <string>

// make sure to update when adding new message types
enum class MsgType { BASIC_MESSAGE };

class Message {
public:
  virtual ~Message() = default;

  // serialise the message to a json string
  virtual std::string serialise() const = 0;

  // create a message object from JSON
  static std::unique_ptr<Message> deserialise(const std::string &json_str);

  // get message type
  virtual MsgType get_type() const = 0;

  const std::string &getSender() const { return sender_; }
  std::chrono::system_clock::time_point getTimestamp() const {
    return timestamp_;
  }

  // utility functions
  static bool is_valid_json(const std::string &json_str);
  static std::string pretty_print(const std::string &json_str);

  // factory method to create a message of the specified type
  static std::unique_ptr<Message> create(const std::string &type,
                                         const std::string &content,
                                         const std::string &sender);

protected:
  Message(const std::string &sender)
      : sender_(sender), timestamp_(std::chrono::system_clock::now()) {}

  std::string sender_;
  std::chrono::system_clock::time_point timestamp_;
};
