// include/common/message_manager.hpp

#pragma once

#include "lumen_header.hpp"
#include "lumen_protocol.hpp"
#include "message.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class UdpServer;
class UdpClient;

using boost::asio::ip::udp;

// manages the serialization and deserialization of application-level messages
class MessageManager {
public:
  MessageManager(boost::asio::io_context &io_context, LumenProtocol &protocol,
                 const std::string &sender_id, UdpServer *server = nullptr,
                 UdpClient *client = nullptr);

  ~MessageManager();

  MessageManager(const MessageManager &) = delete;
  MessageManager &operator=(const MessageManager &) = delete;

  void start();

  void stop();

  // serializes and sends an application-level message via the lumenprotocol
  // layer
  void send_message(const Message &message,
                    const udp::endpoint &recipient = udp::endpoint());

  // sets the callback function to be invoked when a message is successfully
  // deserialized
  void set_message_callback(
      std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
          callback);

  // sends a message directly via udp, bypassing lumenprotocol framing (sends
  // raw json)
  void send_raw_message(const Message &message, const udp::endpoint &recipient);

  // processes a message object that was created from raw json
  void process_raw_json_message(std::unique_ptr<Message> message,
                                const udp::endpoint &sender);

private:
  // internal callback handler called by lumenprotocol upon receiving a framed
  // packet payload
  void handle_lumen_message(const std::vector<uint8_t> &payload,
                            const LumenHeader &header,
                            const udp::endpoint &sender);

  // helper function to convert a string to a vector of bytes
  std::vector<uint8_t> string_to_binary(const std::string &str);

  // helper function to convert a vector of bytes to a string
  std::string binary_to_string(const std::vector<uint8_t> &data);

  // --- member variables ---

  boost::asio::io_context &io_context_;
  LumenProtocol &protocol_;

  std::string sender_id_;

  // callback function pointer (set by application)
  std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
      message_callback_;

  std::mutex callback_mutex_;

  std::atomic<bool> running_;

  // pointers to udp transport layers (optional, used for raw sends)
  UdpServer *server_;
  UdpClient *client_;
};