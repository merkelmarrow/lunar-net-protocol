// src/base/udp_server.cpp

#include "udp_server.hpp"
#include <iostream>

UdpServer::UdpServer(boost::asio::io_context &context, int port)
    : socket_(context, udp::endpoint(udp::v4(), port)), running_(false) {}

UdpServer::~UdpServer() { stop(); }

void UdpServer::start() {
  std::cout << "[SERVER] UDP Server is running..." << std::endl;
  running_ = true;
  receive_data();
}

void UdpServer::stop() {
  running_ = false;

  if (socket_.is_open()) {
    boost::system::error_code ec;
    socket_.close(ec);
    if (ec) {
      std::cerr << "[ERROR] Error closing server socket: " << ec.message()
                << std::endl;
    }
  }
}

void UdpServer::set_receive_callback(
    std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
        callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  receive_callback_ = std::move(callback);
}

void UdpServer::send_data(const std::vector<uint8_t> &data,
                          const udp::endpoint &recipient) {
  socket_.async_send_to(
      boost::asio::buffer(data), recipient,
      [this](boost::system::error_code ec, std::size_t /*length*/) {
        if (ec) {
          std::cerr << "[ERROR] Failed to send data: " << ec.message()
                    << std::endl;
        }
      });
}

void UdpServer::receive_data() {
  if (!running_)
    return;

  socket_.async_receive_from(
      boost::asio::buffer(buffer_), sender_endpoint_,
      [this](boost::system::error_code ec, std::size_t length) {
        if (!ec && running_) {
          // convert the received data to a vector of bytes
          std::vector<uint8_t> received_data(buffer_.data(),
                                             buffer_.data() + length);

          // create local copies to avoid data races
          udp::endpoint endpoint_copy;
          std::function<void(const std::vector<uint8_t> &,
                             const udp::endpoint &)>
              callback_copy;

          {
            std::lock_guard<std::mutex> endpoint_lock(endpoint_mutex_);
            endpoint_copy = sender_endpoint_;
          }

          {
            std::lock_guard<std::mutex> callback_lock(callback_mutex_);
            callback_copy = receive_callback_;
          }

          if (callback_copy) {
            callback_copy(received_data, endpoint_copy);
          }
        }

        if (running_) {
          receive_data(); // continue listening only if still running
        }
      });
}

const udp::endpoint UdpServer::get_sender_endpoint() {
  std::lock_guard<std::mutex> lock(endpoint_mutex_);
  return sender_endpoint_;
}