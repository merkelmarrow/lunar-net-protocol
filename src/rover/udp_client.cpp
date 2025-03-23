// src/rover/udp_client.cpp

#include <boost/asio/ip/udp.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <iostream>

#include <boost/asio/io_context.hpp>

#include "udp_client.hpp"

UdpClient::UdpClient(boost::asio::io_context &io_context)
    : io_context_(io_context),
      // creates a socket bound to ipv4 with a system assigned port (0)
      socket_(io_context, udp::endpoint(udp::v4(), 0)), running_(false),
      receive_buffer_{} {}

UdpClient::~UdpClient() {
  running_ = false;

  if (socket_.is_open()) {
    boost::system::error_code ec;
    socket_.close(ec);
    if (ec) {
      std::cerr << "[ERROR] Error closing socket: " << ec.message()
                << std::endl;
    }
  }
}

void UdpClient::register_base(const std::string &host, int port) {
  try {
    // create a resolver to convert the host name and service to ip address and
    // port
    udp::resolver resolver(io_context_);

    // resolve the endpoint (performs dns lookup if needed)
    // udp::v4() specifies we want an ipv4 address
    auto endpoints = resolver.resolve(udp::v4(), host, std::to_string(port));

    // take first endpoint from results
    base_endpoint_ = *endpoints.begin();

    std::cout << "[CLIENT] Registered base at " << host << ":" << port
              << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Failed to register base endpoint: " << e.what()
              << std::endl;
    throw; // rethrow, caller should handle
  }
}

void UdpClient::send_data(const std::string &message) {
  try {
    // async_send_to initiates an async send operation
    // the lambda function is called when the send completes
    socket_.async_send_to(
        // boost::asio::buffer creates a buffer view of the message data
        boost::asio::buffer(message), base_endpoint_,
        [this, message](const boost::system::error_code &error,
                        std::size_t bytes_sent) {
          // this lambda is called when the send operation completes
          if (error) {
            std::cerr << "[ERROR] Failed to send data: " << error.message()
                      << std::endl;
            throw boost::system::system_error(error);
          }
          std::cout << "[CLIENT] Sent " << bytes_sent << " bytes." << std::endl;
        });
  } catch (const std::exception &error) {
    std::cerr << "[ERROR] Send error: " << error.what() << std::endl;
    throw; // rethrow, caller should handle
  }
}

// sets the callback function
void UdpClient::set_receive_callback(
    std::function<void(const std::string &)> callback) {
  receive_callback_ = std::move(callback);
}