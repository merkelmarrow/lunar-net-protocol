// include/rover/udp_client.hpp

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>

#include "configs.hpp"

using boost::asio::ip::udp;

class UdpClient {
public:
  UdpClient(boost::asio::io_context &io_context);
  ~UdpClient();

  void connect(const std::string &host, int port);

private:
  boost::asio::io_context &io_context_;
  udp::socket socket_;
  udp::endpoint server_endpoint_;
  std::array<char, CLIENT_SOCK_BUF_SIZE> receive_buffer_;
  std::function<void(const std::string &)> receive_callback_;
  bool running_;
};