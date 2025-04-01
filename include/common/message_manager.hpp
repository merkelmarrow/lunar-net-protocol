// include/common/message_manager.hpp

#pragma once

#include "lumen_header.hpp"
#include "lumen_protocol.hpp"
#include "message.hpp"
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class MessageManager {
public:
  MessageManager(boost::asio::io_context &io_context, LumenProtocol &protocol,
                 const std::string &sender_id, UdpServer *server = nullptr,
                 UdpClient *client = nullptr);

  ~MessageManager();

  void start();
  void stop();

  // send a message through the protocol layer
  void send_message(const Message &message,
                    const udp::endpoint &recipient = udp::endpoint());

  void set_message_callback(
      std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
          callback);

  void send_raw_message(const Message &message, const udp::endpoint &recipient);

  void process_raw_json_message(std::unique_ptr<Message> message,
                                const udp::endpoint &sender);

private:
  // handle binary data from lumen protocol layer
  void handle_lumen_message(const std::vector<uint8_t> &payload,
                            const LumenHeader &header,
                            const udp::endpoint &sender);

  // convert between binary data and json strings
  std::vector<uint8_t> string_to_binary(const std::string &str);
  std::string binary_to_string(const std::vector<uint8_t> &data);

  // references to lower layers
  boost::asio::io_context &io_context_;
  LumenProtocol &protocol_;

  std::string sender_id_;

  // callback for received messages
  std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
      message_callback_;

  std::mutex callback_mutex_;

  bool running_;

  UdpServer *server_;
  UdpClient *client_;
};