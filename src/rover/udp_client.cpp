// src/rover/udp_client.cpp

#include <boost/asio/error.hpp>
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
    socket_.async_send_to(
        boost::asio::buffer(message), base_endpoint_,
        [](const boost::system::error_code &error, std::size_t bytes_sent) {
          if (error) {
            // Log the error instead of throwing exception in async callback
            std::cerr << "[ERROR] Failed to send data: " << error.message()
                      << std::endl;
          } else {
            std::cout << "[CLIENT] Sent " << bytes_sent << " bytes."
                      << std::endl;
          }
        });
  } catch (const std::exception &error) {
    std::cerr << "[ERROR] Send error: " << error.what() << std::endl;
    throw; // This is fine since it's in the same thread
  }
}

// sets the callback function
void UdpClient::set_receive_callback(
    std::function<void(const std::string &)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  receive_callback_ = std::move(callback);
}

void UdpClient::start_receive() {
  running_ = true;

  socket_.async_receive_from(boost::asio::buffer(receive_buffer_),
                             base_endpoint_,
                             [this](const boost::system::error_code &error,
                                    std::size_t bytes_transferred) {
                               handle_receive(error, bytes_transferred);
                             });
}

void UdpClient::handle_receive(const boost::system::error_code &error,
                               std::size_t bytes_transferred) {
  if (!error) {
    std::string message(receive_buffer_.data(), bytes_transferred);
    std::cout << "[Client] Received " << bytes_transferred << " bytes."
              << std::endl;

    // Get a thread-safe copy of the callback
    std::function<void(const std::string &)> callback_copy;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback_copy = receive_callback_;
    }

    if (callback_copy) {
      callback_copy(message);
    }

    // Only continue listening if no error occurred
    if (running_) {
      start_receive();
    }
  } else if (error != boost::asio::error::operation_aborted) {
    std::cerr << "[ERROR] Receive error: " << error.message() << std::endl;

    // Avoid immediate retry loop by using a timer
    if (running_) {
      auto timer = std::make_shared<boost::asio::steady_timer>(
          io_context_, boost::asio::chrono::milliseconds(500));

      timer->async_wait([this, timer](const boost::system::error_code &ec) {
        if (!ec && running_) {
          start_receive();
        }
      });
    }
  }
  // no else branch so we don't restart on operation_aborted
}