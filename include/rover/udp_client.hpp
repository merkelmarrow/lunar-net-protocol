// include/rover/udp_client.hpp

#pragma once

#include "configs.hpp"

#include <array>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/system/error_code.hpp>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using boost::asio::ip::udp;

// provides asynchronous udp client functionality using boost.asio
class UdpClient {
public:
  UdpClient(boost::asio::io_context &io_context);

  ~UdpClient();

  UdpClient(const UdpClient &) = delete;
  UdpClient &operator=(const UdpClient &) = delete;

  // resolves the base station's hostname/ip and port, storing the endpoint
  void register_base(const std::string &host, int port);

  // sends data asynchronously to the registered base station endpoint
  void send_data(const std::vector<uint8_t> &data);

  // sends data asynchronously to a specific recipient endpoint
  void send_data_to(const std::vector<uint8_t> &data,
                    const udp::endpoint &recipient);

  // sends data asynchronously as a broadcast message on the local network
  void send_broadcast_data(
      const std::vector<uint8_t> &data, int broadcast_port,
      const std::string &broadcast_address_str = "255.255.255.255");

  // sets the callback function to be invoked when data is received
  void set_receive_callback(
      std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
          callback);

  // starts the asynchronous receive loop
  void start_receive();

  // stops the asynchronous receive loop and closes the socket
  void stop_receive();

  // gets the resolved endpoint of the registered base station
  const udp::endpoint &get_base_endpoint() const;

private:
  // internal callback handler for completed async_receive_from operations
  void handle_receive(const boost::system::error_code &error,
                      std::size_t bytes_transferred);

  // initiates a single asynchronous receive operation
  void do_receive();

  // attempts to enable the broadcast socket option
  void enable_broadcast();

  boost::asio::io_context &io_context_;
  udp::socket socket_;

  udp::endpoint base_endpoint_;
  udp::endpoint receive_endpoint_; // stores the sender's endpoint from the last
                                   // received packet

  std::array<char, CLIENT_SOCK_BUF_SIZE> receive_buffer_;

  // callback signature modified to include sender endpoint
  std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
      receive_callback_;

  // flag to control the receive loop
  std::atomic<bool> running_;
  // mutex to protect access to receive_callback_
  std::mutex callback_mutex_;
};