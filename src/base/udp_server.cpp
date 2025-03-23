// src/base/udp_server.cpp

#include "udp_server.hpp"
#include <iostream>

UdpServer::UdpServer(boost::asio::io_context &context, int port)
    : socket_(context, udp::endpoint(udp::v4(), port)) {}

void UdpServer::start() {
  std::cout << "[SERVER] UDP Server is running..." << std::endl;
  receive_data();
}

void UdpServer::set_receive_callback(
    std::function<void(const std::string &)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  receive_callback_ = std::move(callback);
}

void UdpServer::send_data(const std::string &message,
                          const udp::endpoint &recipient) {
  socket_.async_send_to(
      boost::asio::buffer(message), recipient,
      [this](boost::system::error_code ec, std::size_t /*length*/) {
        if (ec) {
          std::cerr << "[ERROR] Failed to send data: " << ec.message()
                    << std::endl;
        }
      });
}

void UdpServer::receive_data() {
  socket_.async_receive_from(
      boost::asio::buffer(buffer_), sender_endpoint_,
      [this](boost::system::error_code ec, std::size_t length) {
        if (!ec) {
          std::string received_message(buffer_.data(), length);

          // Create local copies to avoid data races
          udp::endpoint endpoint_copy;
          std::function<void(const std::string &)> callback_copy;

          {
            std::lock_guard<std::mutex> endpoint_lock(endpoint_mutex_);
            endpoint_copy = sender_endpoint_;
          }

          {
            std::lock_guard<std::mutex> callback_lock(callback_mutex_);
            callback_copy = receive_callback_;
          }

          std::cout << "[RECEIVED] Message: " << received_message << std::endl;

          if (callback_copy) {
            callback_copy(received_message);
          }
        }
        receive_data(); // Continue listening
      });
}

const udp::endpoint UdpServer::get_sender_endpoint() {
  std::lock_guard<std::mutex> lock(endpoint_mutex_);
  return sender_endpoint_;
}

UdpServer::~UdpServer() {
  if (socket_.is_open()) {
    boost::system::error_code ec;
    socket_.close(ec);
    if (ec) {
      std::cerr << "[ERROR] Error closing server socket: " << ec.message()
                << std::endl;
    }
  }
}