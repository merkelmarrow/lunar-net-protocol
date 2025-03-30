// src/common/message_manager.cpp

#include "message_manager.hpp"
#include "lumen_header.hpp"
#include "message.hpp"
#include <iostream>
#include <vector>

MessageManager::MessageManager(boost::asio::io_context &io_context,
                               LumenProtocol &protocol,
                               const std::string &sender_id)
    : io_context_(io_context), protocol_(protocol), sender_id_(sender_id),
      running_(false) {}

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
            << " to " << recipient << std::endl;
}