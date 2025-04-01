// src/common/message_manager.cpp

#include "message_manager.hpp"
#include "lumen_header.hpp"
#include "message.hpp"
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <vector>

MessageManager::MessageManager(boost::asio::io_context &io_context,
                               LumenProtocol &protocol,
                               const std::string &sender_id, UdpServer *server,
                               UdpClient *client)
    : io_context_(io_context), protocol_(protocol), sender_id_(sender_id),
      running_(false), server_(server), client_(client) {}

MessageManager::~MessageManager() { stop(); }

void MessageManager::start() {
  if (running_)
    return;

  running_ = true;

  // Set up the callback to handle messages from the LUMEN protocol
  protocol_.set_message_callback([this](const std::vector<uint8_t> &payload,
                                        const LumenHeader &header,
                                        const udp::endpoint &sender) {
    handle_lumen_message(payload, header, sender);
  });

  std::cout << "[MESSAGE MANAGER] Started for sender ID: " << sender_id_
            << std::endl;
}

void MessageManager::stop() {
  if (!running_)
    return;

  running_ = false;
  std::cout << "[MESSAGE MANAGER] Stopped for sender ID: " << sender_id_
            << std::endl;
}

void MessageManager::send_message(const Message &message,
                                  const udp::endpoint &recipient) {
  if (!running_) {
    std::cerr << "[ERROR] Message manager not running." << std::endl;
    return;
  }

  // serialise the message to json
  std::string json_str = message.serialise();

  // convert to binary
  std::vector<uint8_t> payload = string_to_binary(json_str);

  // get protocol parameters
  LumenHeader::MessageType lumen_type = message.get_lumen_type();
  LumenHeader::Priority priority = message.get_lumen_priority();

  // send through the protocol
  protocol_.send_message(payload, lumen_type, priority, recipient);

  std::cout << "[MESSAGE MANAGER] Sent message type: " << message.get_type()
            << " to "
            << (recipient.address().is_unspecified()
                    ? (client_ ? client_->get_base_endpoint() : udp::endpoint())
                    : recipient)
            << std::endl;
}

void MessageManager::set_message_callback(
    std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
        callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  message_callback_ = std::move(callback);
}

void MessageManager::handle_lumen_message(const std::vector<uint8_t> &payload,
                                          const LumenHeader &header,
                                          const udp::endpoint &sender) {
  try {
    // Check if this might be a raw JSON message (special case)
    if (header.get_sequence() == 0 && !payload.empty() && payload[0] == '{') {
      // Convert binary to string
      std::string json_str = binary_to_string(payload);

      // Pretty print the JSON if it's valid
      if (Message::is_valid_json(json_str)) {
        std::string pretty_json;
        try {
          pretty_json = Message::pretty_print(json_str);
          std::cout << "[MESSAGE MANAGER] Received raw JSON message: \n"
                    << pretty_json << std::endl;
        } catch (const std::exception &e) {
          std::cout << "[MESSAGE MANAGER] Received raw JSON message (failed to "
                       "pretty print): "
                    << (json_str.length() > 100
                            ? json_str.substr(0, 100) + "..."
                            : json_str)
                    << std::endl;
        }

        // Attempt to parse as a Message
        auto message = Message::deserialise(json_str);

        // Call the callback if it's been set
        std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
            callback_copy;
        {
          std::lock_guard<std::mutex> lock(callback_mutex_);
          callback_copy = message_callback_;
        }

        if (callback_copy) {
          callback_copy(std::move(message), sender);
        }
      } else {
        std::cerr << "[ERROR] Received raw JSON message is not valid: "
                  << (json_str.length() > 100 ? json_str.substr(0, 100) + "..."
                                              : json_str)
                  << std::endl;
      }

      return;
    }

    // Regular message handling continues...
    std::string json_str = binary_to_string(payload);

    // check if it's a valid json msg
    if (!Message::is_valid_json(json_str)) {
      std::cerr << "[ERROR] Received invalid JSON message." << std::endl;
      return;
    }

    // deserialise into the appropriate message type
    auto message = Message::deserialise(json_str);

    // call the callback if it's been set
    std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
        callback_copy;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback_copy = message_callback_;
    }

    if (callback_copy) {
      callback_copy(std::move(message), sender);
    }
  } catch (const std::exception &error) {
    std::cerr << "[ERROR] Failed to process message: " << error.what()
              << std::endl;
  }
}

std::vector<uint8_t> MessageManager::string_to_binary(const std::string &str) {
  return std::vector<uint8_t>(str.begin(), str.end());
}

std::string MessageManager::binary_to_string(const std::vector<uint8_t> &data) {
  return std::string(data.begin(), data.end());
}

void MessageManager::send_raw_message(const Message &message,
                                      const udp::endpoint &recipient) {
  if (!running_) {
    std::cerr << "[ERROR] Message manager not running." << std::endl;
    return;
  }

  // serialize the message to JSON
  std::string json_str = message.serialise();

  // convert to binary
  std::vector<uint8_t> data = string_to_binary(json_str);

  // send directly via appropriate channel
  if (server_) {
    server_->send_data(data, recipient);
    std::cout << "[MESSAGE MANAGER] Sent raw message type: "
              << message.get_type() << " to " << recipient << std::endl;
  } else if (client_) {
    client_->send_data_to(data, recipient);
    std::cout << "[MESSAGE MANAGER] Sent raw message type: "
              << message.get_type() << " to " << recipient << std::endl;
  } else {
    std::cerr << "[ERROR] No sender available for raw message" << std::endl;
  }
}

void MessageManager::process_raw_json_message(std::unique_ptr<Message> message,
                                              const udp::endpoint &sender) {
  // Call the callback directly
  std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
      callback_copy;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_copy = message_callback_;
  }

  if (callback_copy) {
    callback_copy(std::move(message), sender);
  }
}