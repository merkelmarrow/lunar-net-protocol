// src/rover/udp_client.cpp

#include <iostream>

#include <boost/asio/io_context.hpp>

#include "udp_client.hpp"

UdpClient::UdpClient(boost::asio::io_context &io_context)
    : io_context_(io_context), socket_(io_context, udp::endpoint(udp::v4(), 0)),
      running_(false) {}

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