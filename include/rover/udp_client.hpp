// include/rover/udp_client.hpp

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <functional>

#include "configs.hpp"

using boost::asio::ip::udp;

class UdpClient {
public:
  // constructor creates a UDP client connected to the provided io_context
  // the io_context will drive all async operations, it represents an
  // event loop where tasks are scheduled and executed.
  // io_context needs to remain valid throughout the lifetime of UdpClient obj
  UdpClient(boost::asio::io_context &io_context);

  // closes socket
  ~UdpClient();

  // sets up a destination endpoint (of the base station) for future
  // communications
  // host is the ip address of the server
  // port is the port number on which the server is listening
  void register_base(const std::string &host, int port);

  // send data to the server asynchronously
  // method returns immediately, the actual operation happens in the background
  // on the io_context thread.
  // message is the data to send
  // the method throws boost::system::system_error if the async operation fails
  // to start
  void send_data(const std::string &message);

  // sets the callback to receive messages
  // the callback function will be called whenever a message is received
  // the callback should handle/process the message string
  void set_receive_callback(std::function<void(const std::string &)> callback);

  void start_receive();

private:
  void handle_receive(const boost::system::error_code &error,
                      std::size_t bytes_transferred);

  boost::asio::io_context &io_context_;
  udp::socket socket_;
  udp::endpoint base_endpoint_;
  std::array<char, CLIENT_SOCK_BUF_SIZE> receive_buffer_;
  std::function<void(const std::string &)> receive_callback_;
  bool running_;
};