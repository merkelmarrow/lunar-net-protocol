// src/common/message_manager.cpp

#include "message_manager.hpp"
#include "lumen_header.hpp"
#include "message.hpp"
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <vector>

// passes udpserver/udpclient pointers for send_raw_message
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

  // register this messagemanager's handler with the lumenprotocol layer
  // this lambda will be called by lumenprotocol when it has successfully parsed
  // a packet
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

  try {
    std::string json_str = message.serialise();
    std::vector<uint8_t> payload = string_to_binary(json_str);

    LumenHeader::MessageType lumen_type = message.get_lumen_type();
    LumenHeader::Priority priority = message.get_lumen_priority();

    protocol_.send_message(payload, lumen_type, priority, recipient);

    udp::endpoint log_recipient = recipient;
    if (log_recipient.address().is_unspecified() && client_) {
      // if recipient is default and we are a client, log client's base endpoint
      try {
        log_recipient = client_->get_base_endpoint();
      } catch (const std::runtime_error &e) {
        std::cerr
            << "[WARN] MessageManager logging: Could not get base endpoint - "
            << e.what() << std::endl;
      }
    }

  } catch (const std::exception &e) {
    std::cerr << "[ERROR] MessageManager failed to send message type "
              << message.get_type() << ": " << e.what() << std::endl;
  }
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
  if (!running_)
    return;

  try {
    std::string json_str = binary_to_string(payload);

    if (!Message::is_valid_json(json_str)) {
      std::cerr << "[ERROR] MessageManager received invalid JSON payload from "
                   "protocol. Seq: "
                << static_cast<int>(header.get_sequence()) << " from " << sender
                << std::endl;
      return;
    }

    // use message factory to deserialize json into specific message subclass
    // object
    auto message = Message::deserialise(json_str);
    if (!message) {
      std::cerr
          << "[ERROR] MessageManager failed to deserialize valid JSON. Seq: "
          << static_cast<int>(header.get_sequence()) << " from " << sender
          << std::endl;
      return;
    }

    // get thread-safe copy of application-level callback
    std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
        app_callback_copy;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      app_callback_copy = message_callback_;
    }

    // if application callback registered, pass deserialized message object up
    if (app_callback_copy) {
      // move ownership of unique_ptr to application layer
      app_callback_copy(std::move(message), sender);
    } else {
      std::cout << "[MESSAGE MANAGER] Warning: No application callback set to "
                   "handle message type "
                << (message ? message->get_type() : "unknown") << " from "
                << sender << std::endl;
    }
  } catch (const std::exception &error) {
    std::cerr << "[ERROR] MessageManager failed to process received lumen "
                 "message payload. Seq: "
              << static_cast<int>(header.get_sequence()) << " from " << sender
              << ". Error: " << error.what() << std::endl;
  }
}

std::vector<uint8_t> MessageManager::string_to_binary(const std::string &str) {
  return std::vector<uint8_t>(str.begin(), str.end());
}

std::string MessageManager::binary_to_string(const std::vector<uint8_t> &data) {
  return std::string(data.begin(), data.end());
}

// sends a message directly via udp, bypassing the lumenprotocol layer
void MessageManager::send_raw_message(const Message &message,
                                      const udp::endpoint &recipient) {
  if (!running_) {
    std::cerr << "[ERROR] Message manager not running." << std::endl;
    return;
  }

  try {
    std::string json_str = message.serialise();
    std::vector<uint8_t> data = string_to_binary(json_str);

    // use appropriate udp transport pointer to send raw data
    if (server_) {
      server_->send_data(data, recipient);
    } else if (client_) { // rover mode
      // udpclient needs to differentiate sending to default base vs. specific endpoint
      bool sending_to_base = false;
      try {
        sending_to_base = (recipient.address().is_unspecified() ||
                           recipient == client_->get_base_endpoint());
      } catch (const std::runtime_error &e) {
        std::cerr
            << "[WARN] MessageManager raw send: Could not get base endpoint - "
            << e.what() << std::endl;
        sending_to_base =
            recipient.address().is_unspecified(); // assume base if unspecified
      }

      if (sending_to_base) {
        client_->send_data(data);
      } else {
        client_->send_data_to(data,
                              recipient);
      }
    } else {
      std::cerr << "[ERROR] MessageManager: No UdpServer or UdpClient "
                   "available for send_raw_message."
                << std::endl;
    }
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] MessageManager failed to send raw message type "
              << message.get_type() << ": " << e.what() << std::endl;
  }
}

// processes a message object deserialized directly from raw json (bypassing lumenprotocol)
void MessageManager::process_raw_json_message(std::unique_ptr<Message> message,
                                              const udp::endpoint &sender) {
  if (!running_ || !message)
    return;

  std::cout << "[MESSAGE MANAGER] Processing raw JSON message type: "
            << message->get_type() << " from " << sender << std::endl;

  // get thread-safe copy of application callback
  std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
      callback_copy;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_copy = message_callback_;
  }

  // pass message up to application layer if callback set
  if (callback_copy) {
    callback_copy(std::move(message), sender);
  } else {
    std::cout << "[MESSAGE MANAGER] Warning: No application callback set to "
                 "handle raw JSON message type "
              << message->get_type() << " from " << sender << std::endl;
  }
}