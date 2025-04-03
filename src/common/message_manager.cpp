// src/common/message_manager.cpp

#include "message_manager.hpp"
#include "lumen_header.hpp"
#include "message.hpp"
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <vector>

// note: passes optional udpserver/udpclient pointers for send_raw_message
// functionality
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
    // convert application-level message object into json string
    std::string json_str = message.serialise();
    // convert json string into binary payload for protocol layer
    std::vector<uint8_t> payload = string_to_binary(json_str);

    // extract protocol-specific details (type, priority) from message object
    LumenHeader::MessageType lumen_type = message.get_lumen_type();
    LumenHeader::Priority priority = message.get_lumen_priority();

    // pass binary payload and metadata down to lumenprotocol layer
    protocol_.send_message(payload, lumen_type, priority, recipient);

    // logging: determine target endpoint for logging
    udp::endpoint log_recipient = recipient;
    if (log_recipient.address().is_unspecified() && client_) {
      // if recipient is default and we are a client, log client's base endpoint
      try { // add try-catch in case base endpoint isn't resolved yet
        log_recipient = client_->get_base_endpoint();
      } catch (const std::runtime_error &e) {
        std::cerr
            << "[WARN] MessageManager logging: Could not get base endpoint - "
            << e.what() << std::endl;
        // keep log_recipient as unspecified
      }
    }
    // removed verbose logging here

  } catch (const std::exception &e) {
    std::cerr << "[ERROR] MessageManager failed to send message type "
              << message.get_type() << ": " << e.what() << std::endl;
  }
}

void MessageManager::set_message_callback(
    std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
        callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  // store callback provided by application layer (e.g., basestation or rover)
  message_callback_ = std::move(callback);
}

// this function is called by lumenprotocol when it delivers a validated payload
void MessageManager::handle_lumen_message(const std::vector<uint8_t> &payload,
                                          const LumenHeader &header,
                                          const udp::endpoint &sender) {
  if (!running_)
    return;

  try {
    // convert binary payload received from lumenprotocol back into string
    std::string json_str = binary_to_string(payload);

    // validation to ensure payload is valid json
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
    // catch potential errors during deserialization or other processing
    std::cerr << "[ERROR] MessageManager failed to process received lumen "
                 "message payload. Seq: "
              << static_cast<int>(header.get_sequence()) << " from " << sender
              << ". Error: " << error.what() << std::endl;
  }
}

// helper function for simple string-to-byte conversion
std::vector<uint8_t> MessageManager::string_to_binary(const std::string &str) {
  return std::vector<uint8_t>(str.begin(), str.end());
}

// helper function for simple byte-to-string conversion
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
    // serialize message to json, as that's the expected raw format
    std::string json_str = message.serialise();
    std::vector<uint8_t> data = string_to_binary(json_str);

    // use appropriate udp transport pointer (if available) to send raw data
    if (server_) { // if configured with server (basestation mode)
      server_->send_data(data, recipient);
    } else if (client_) { // if configured with client (rover mode)
      // udpclient needs to differentiate sending to default base vs. specific
      // endpoint
      bool sending_to_base = false;
      try { // handle case where base endpoint might not be resolved yet
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
        client_->send_data(data); // send to default registered base
      } else {
        client_->send_data_to(data,
                              recipient); // send to specific non-base endpoint
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

// processes a message object deserialized directly from raw json (bypassing
// lumenprotocol)
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