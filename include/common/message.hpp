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

  // pure virtual method to serialize the message content into a json string
  virtual std::string serialise() const = 0;

  // pure virtual method to get the specific type identifier string for the
  // message subclass
  virtual std::string get_type() const = 0;

  const std::string &get_sender() const { return sender_; }

  std::chrono::system_clock::time_point get_timestamp() const {
    return timestamp_;
  }

  // pure virtual method to get the appropriate lumen protocol priority
  virtual LumenHeader::Priority get_lumen_priority() const = 0;

  // pure virtual method to get the appropriate lumen protocol message type
  virtual LumenHeader::MessageType get_lumen_type() const = 0;

  // --- static utility & factory methods ---

  // utility function to check if a given string is structurally valid json
  static bool is_valid_json(const std::string &json_str);

  // utility function to format a json string with indentation
  static std::string pretty_print(const std::string &json_str);

  // factory method to deserialize a json string into a specific message
  // subclass object
  static std::unique_ptr<Message> deserialise(const std::string &json_str);

protected:
  // protected constructor for base class initialization by derived classes
  Message(const std::string &sender)
      : sender_(sender), timestamp_(std::chrono::system_clock::now()) {}

  // protected method allowing derived classes to set the timestamp from parsed
  // json
  void set_timestamp(const std::chrono::system_clock::time_point &timestamp) {
    timestamp_ = timestamp;
  };

  // --- common member variables ---
  std::string sender_;
  std::chrono::system_clock::time_point timestamp_;
};