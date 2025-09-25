// include/common/message.hpp

#pragma once

#include "lumen_header.hpp"
#include <chrono>
#include <memory>
#include <string>

// abstract base class for all application-level message types
class Message {
public:
  virtual ~Message() = default;

  // virtual method to serialize the message content into a json string
  virtual std::string serialise() const = 0;

  // virtual method to get the specific type identifier string for the message subclass
  virtual std::string get_type() const = 0;

  const std::string &get_sender() const { return sender_; }

  std::chrono::system_clock::time_point get_timestamp() const {
    return timestamp_;
  }

  virtual LumenHeader::Priority get_lumen_priority() const = 0;

  virtual LumenHeader::MessageType get_lumen_type() const = 0;


  static bool is_valid_json(const std::string &json_str);

  // utility function to format a json string with indentation
  static std::string pretty_print(const std::string &json_str);

  // factory method to deserialize a json string into a specific message subclass object
  static std::unique_ptr<Message> deserialise(const std::string &json_str);

protected:
  Message(const std::string &sender)
      : sender_(sender), timestamp_(std::chrono::system_clock::now()) {}

  // protected method allowing derived classes to set the timestamp from parsed json
  void set_timestamp(const std::chrono::system_clock::time_point &timestamp) {
    timestamp_ = timestamp;
  };

  std::string sender_;
  std::chrono::system_clock::time_point timestamp_;
};