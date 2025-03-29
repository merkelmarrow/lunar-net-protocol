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
  void stop();

  // set callback for receiving binary data
  void set_receive_callback(
      std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
          callback);

  // send binary data
  void send_data(const std::vector<uint8_t> &data,
                 const udp::endpoint &recipient);

  // get the endpoint of the most recent sender
  const udp::endpoint get_sender_endpoint();

private:
  // async receive
  void receive_data();

  udp::socket socket_;
  udp::endpoint sender_endpoint_;
  std::array<char, SERVER_SOCK_BUF_SIZE> buffer_;
  std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
      receive_callback_;

  std::mutex endpoint_mutex_;
  std::mutex callback_mutex_;
  bool running_;
};