// include/base/udp_server.hpp

#pragma once

#include "configs.hpp"
#include <array>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <functional>
#include <mutex>
#include <vector>

using boost::asio::ip::udp;

// provides asynchronous udp server functionality using boost.asio for the base
// station
class UdpServer {
public:
  UdpServer(boost::asio::io_context &context, int port);

  ~UdpServer();

  UdpServer(const UdpServer &) = delete;
  UdpServer &operator=(const UdpServer &) = delete;

  void start();

  void stop();

  // sets the callback function to be invoked when a udp datagram is received
  void set_receive_callback(
      std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
          callback);

  // sends data asynchronously to a specific recipient endpoint
  void send_data(const std::vector<uint8_t> &data,
                 const udp::endpoint &recipient);

  void scan_for_rovers(int discovery_port, const std::string &message,
                       const std::string &sender_id);

  // gets the udp endpoint of the sender of the most recently received datagram
  const udp::endpoint get_sender_endpoint();

private:
  // initiates a single asynchronous receive operation
  void receive_data();

  udp::socket socket_;
  udp::endpoint sender_endpoint_; // stores the endpoint of the client from the
                                  // last successful receive

  std::array<char, SERVER_SOCK_BUF_SIZE> buffer_;

  // callback function provided by the user (e.g., lumenprotocol)
  std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
      receive_callback_;

  std::mutex endpoint_mutex_;
  std::mutex callback_mutex_;
  std::atomic<bool> running_;

  boost::asio::io_context &io_context_;
};