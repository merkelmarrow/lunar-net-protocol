// src/rover/udp_client.cpp

#include <boost/asio/ip/udp.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <iostream>

#include <boost/asio/io_context.hpp>

#include "udp_client.hpp"

UdpClient::UdpClient(boost::asio::io_context &io_context)
    : io_context_(io_context), socket_(io_context, udp::endpoint(udp::v4(), 0)),
      running_(false), receive_buffer_{} {}

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
    udp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(udp::v4(), host, std::to_string(port));

    base_endpoint_ = *endpoints.begin();

    std::cout << "[CLIENT] Registered base at " << host << ":" << port
              << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Failed to register base endpoint: " << e.what()
              << std::endl;
    throw;
  }
}

void UdpClient::send_data(const std::string &message) {
  try {
    socket_.async_send_to(
        boost::asio::buffer(message), base_endpoint_,
        [this, message](const boost::system::error_code &error,
                        std::size_t bytes_sent) {
          if (error) {
            std::cerr << "[ERROR] Failed to send data: " << error.message()
                      << std::endl;
            throw boost::system::system_error(error);
          }
          std::cout << "[CLIENT] Sent " << bytes_sent << " bytes." << std::endl;
        });
  } catch (const std::exception &error) {
    std::cerr << "[ERROR] Send error: " << error.what() << std::endl;
    throw;
  }
}