// src/common/message_manager.cpp

#include "message_manager.hpp"
#include "lumen_header.hpp" // Used for header info in callbacks
#include "message.hpp"      // Used for Message factory and interface
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <vector>

// Note: Passes optional UdpServer/UdpClient pointers for send_raw_message
// functionality.
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

  // Register this MessageManager's handler with the LumenProtocol layer.
  // This lambda will be called by LumenProtocol when it has successfully
  // parsed a packet's payload and header.
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
  // Optionally, could unregister the callback from protocol_ here if needed.
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
    // Convert the application-level Message object into a JSON string.
    std::string json_str = message.serialise();
    // Convert the JSON string into a binary payload for the protocol layer.
    std::vector<uint8_t> payload = string_to_binary(json_str);

    // Extract protocol-specific details (type, priority) from the Message
    // object.
    LumenHeader::MessageType lumen_type = message.get_lumen_type();
    LumenHeader::Priority priority = message.get_lumen_priority();

    // Pass the binary payload and metadata down to the LumenProtocol layer.
    protocol_.send_message(payload, lumen_type, priority, recipient);

    // Logging: Determine the target endpoint for logging clarity.
    udp::endpoint log_recipient = recipient;
    if (log_recipient.address().is_unspecified() && client_) {
      // If recipient is default and we are a client, log the client's base
      // endpoint.
      log_recipient = client_->get_base_endpoint();
    }
    // std::cout << "[MESSAGE MANAGER] Sent message type: " <<
    // message.get_type() << " via protocol to " << log_recipient << std::endl;
    // // Reduced verbosity

  } catch (const std::exception &e) {
    std::cerr << "[ERROR] MessageManager failed to send message type "
              << message.get_type() << ": " << e.what() << std::endl;
  }
}

void MessageManager::set_message_callback(
    std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
        callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  // Store the callback provided by the application layer (e.g., BaseStation or
  // Rover).
  message_callback_ = std::move(callback);
}

// This function is called BY LumenProtocol when it delivers a validated
// payload.
void MessageManager::handle_lumen_message(const std::vector<uint8_t> &payload,
                                          const LumenHeader &header,
                                          const udp::endpoint &sender) {
  if (!running_)
    return;

  try {
    // Convert the binary payload received from LumenProtocol back into a
    // string.
    std::string json_str = binary_to_string(payload);

    // Basic validation to ensure the payload is potentially valid JSON.
    if (!Message::is_valid_json(json_str)) {
      std::cerr << "[ERROR] MessageManager received invalid JSON payload from "
                   "protocol. Seq: "
                << static_cast<int>(header.get_sequence()) << " from " << sender
                << std::endl;
      return;
    }

    // Use the Message factory to deserialize the JSON into the specific Message
    // subclass object.
    auto message = Message::deserialise(json_str);
    if (!message) {
      std::cerr
          << "[ERROR] MessageManager failed to deserialize valid JSON. Seq: "
          << static_cast<int>(header.get_sequence()) << " from " << sender
          << std::endl;
      return;
    }

    // Get a thread-safe copy of the application-level callback.
    std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
        app_callback_copy;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      app_callback_copy = message_callback_;
    }

    // If an application callback is registered, pass the deserialized message
    // object up.
    if (app_callback_copy) {
      // Move ownership of the unique_ptr to the application layer.
      app_callback_copy(std::move(message), sender);
    } else {
      // This might be normal if the application hasn't set a handler yet, or an
      // issue.
      std::cout << "[MESSAGE MANAGER] Warning: No application callback set to "
                   "handle message type "
                << (message ? message->get_type() : "unknown") << " from "
                << sender << std::endl;
    }
  } catch (const std::exception &error) {
    // Catch potential errors during deserialization or other processing.
    std::cerr << "[ERROR] MessageManager failed to process received lumen "
                 "message payload. Seq: "
              << static_cast<int>(header.get_sequence()) << " from " << sender
              << ". Error: " << error.what() << std::endl;
  }
}

// Helper function for simple string-to-byte conversion.
std::vector<uint8_t> MessageManager::string_to_binary(const std::string &str) {
  return std::vector<uint8_t>(str.begin(), str.end());
}

// Helper function for simple byte-to-string conversion.
std::string MessageManager::binary_to_string(const std::vector<uint8_t> &data) {
  return std::string(data.begin(), data.end());
}

// Sends a message directly via UDP, bypassing the LumenProtocol layer.
void MessageManager::send_raw_message(const Message &message,
                                      const udp::endpoint &recipient) {
  if (!running_) {
    std::cerr << "[ERROR] Message manager not running." << std::endl;
    return;
  }

  try {
    // Serialize the message to JSON, as that's the expected raw format in this
    // context.
    std::string json_str = message.serialise();
    std::vector<uint8_t> data = string_to_binary(json_str);

    // Use the appropriate UDP transport pointer (if available) to send the raw
    // data.
    if (server_) { // If configured with a server (BaseStation mode)
      server_->send_data(data, recipient);
      // std::cout << "[MESSAGE MANAGER] Sent raw (JSON) message type: " <<
      // message.get_type() << " via UdpServer to " << recipient << std::endl;
      // // Reduced verbosity
    } else if (client_) { // If configured with a client (Rover mode)
      // UdpClient needs to differentiate sending to default base vs. specific
      // endpoint
      if (recipient.address().is_unspecified() ||
          recipient == client_->get_base_endpoint()) {
        client_->send_data(data); // Send to default registered base
        // std::cout << "[MESSAGE MANAGER] Sent raw (JSON) message type: " <<
        // message.get_type() << " via UdpClient to base " <<
        // client_->get_base_endpoint() << std::endl; // Reduced verbosity
      } else {
        client_->send_data_to(
            data, recipient); // Send to a specific non-base endpoint
        // std::cout << "[MESSAGE MANAGER] Sent raw (JSON) message type: " <<
        // message.get_type() << " via UdpClient to specific " << recipient <<
        // std::endl; // Reduced verbosity
      }
    } else {
      // Should not happen if constructor logic is correct.
      std::cerr << "[ERROR] MessageManager: No UdpServer or UdpClient "
                   "available for send_raw_message."
                << std::endl;
    }
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] MessageManager failed to send raw message type "
              << message.get_type() << ": " << e.what() << std::endl;
  }
}

// Processes a message object that was deserialized directly from raw JSON
// (bypassing LumenProtocol).
void MessageManager::process_raw_json_message(std::unique_ptr<Message> message,
                                              const udp::endpoint &sender) {
  if (!running_ || !message)
    return;

  std::cout << "[MESSAGE MANAGER] Processing raw JSON message type: "
            << message->get_type() << " from " << sender << std::endl;

  // Get a thread-safe copy of the application callback.
  std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
      callback_copy;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_copy = message_callback_;
  }

  // Pass the message up to the application layer if a callback is set.
  if (callback_copy) {
    callback_copy(std::move(message), sender);
  } else {
    std::cout << "[MESSAGE MANAGER] Warning: No application callback set to "
                 "handle raw JSON message type "
              << message->get_type() << " from " << sender << std::endl;
  }
}