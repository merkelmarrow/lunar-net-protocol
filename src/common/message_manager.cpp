// src/common/message_manager.cpp

#include "message_manager.hpp"
#include "message.hpp"
#include <iostream>

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