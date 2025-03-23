// include/base/udp_server.hpp

#pragma once

#include "configs.hpp"
#include <boost/asio.hpp>
#include <functional>
#include <mutex>

using boost::asio::ip::udp;

class UdpServer {
public:
  UdpServer(boost::asio::io_context &context, int port);
  ~UdpServer();
  void start();
  void set_receive_callback(std::function<void(const std::string &)> callback);
  void send_data(const std::string &message, const udp::endpoint &recipient);

  const udp::endpoint get_sender_endpoint();

private:
  void receive_data();

  udp::socket socket_;
  udp::endpoint sender_endpoint_;
  std::array<char, SERVER_SOCK_BUF_SIZE> buffer_;
  std::function<void(const std::string &)> receive_callback_;

  std::mutex endpoint_mutex_;
  std::mutex callback_mutex_;
};