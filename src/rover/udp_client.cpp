// src/rover/udp_client.cpp

#include "udp_client.hpp"
#include "configs.hpp"

#include <atomic>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

UdpClient::UdpClient(boost::asio::io_context &io_context)
    : io_context_(io_context),
      socket_(io_context,
              udp::endpoint(udp::v4(), 0)), // bind to any available port
      running_(false) {
  enable_broadcast(); // attempt on creation
}

UdpClient::~UdpClient() { stop_receive(); }

void UdpClient::enable_broadcast() {
  boost::system::error_code ec;
  socket_.set_option(boost::asio::socket_base::broadcast(true), ec);
  if (ec) {
    std::cerr << "[CLIENT] Warning: Failed to set broadcast socket option: "
              << ec.message() << std::endl;
  } else {
    std::cout << "[CLIENT] Broadcast socket option enabled." << std::endl;
  }
}

void UdpClient::register_base(const std::string &host, int port) {
  try {
    udp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(udp::v4(), host, std::to_string(port));

    if (endpoints.empty()) {
      throw std::runtime_error("Could not resolve host: " + host);
    }
    base_endpoint_ = *endpoints.begin();
    std::cout << "[CLIENT] Registered base station endpoint: " << base_endpoint_
              << std::endl;
  } catch (const boost::system::system_error &e) {
    std::cerr << "[ERROR] Failed to resolve or register base endpoint (" << host
              << ":" << port << "): " << e.what() << std::endl;
    throw;
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Unexpected error during base registration: "
              << e.what() << std::endl;
    throw;
  }
}

// sends data to the default registered base station
void UdpClient::send_data(const std::vector<uint8_t> &data) {
  if (base_endpoint_.address().is_unspecified()) {
    std::cerr << "[ERROR] UdpClient::send_data: Base endpoint is not "
                 "initialized. Call register_base() first."
              << std::endl;
    throw std::runtime_error("Base endpoint not initialized for UdpClient");
  }

  socket_.async_send_to(boost::asio::buffer(data), base_endpoint_,
                        [this](const boost::system::error_code &error,
                               std::size_t /*bytes_sent*/) {
                          if (error) {
                            std::cerr << "[ERROR] UdpClient::send_data failed: "
                                      << error.message() << std::endl;
                          }
                        });
}

// sends data to a specific, provided recipient endpoint
void UdpClient::send_data_to(const std::vector<uint8_t> &data,
                             const udp::endpoint &recipient) {
  if (recipient.address().is_unspecified() || recipient.port() == 0) {
    std::cerr << "[ERROR] UdpClient::send_data_to: Invalid recipient endpoint "
                 "provided."
              << std::endl;
    return;
  }

  socket_.async_send_to(
      boost::asio::buffer(data), recipient,
      [this, recipient](const boost::system::error_code &error,
                        std::size_t /*bytes_sent*/) {
        if (error) {
          std::cerr << "[ERROR] UdpClient::send_data_to failed for "
                    << recipient << ": " << error.message() << std::endl;
        }
      });
}

void UdpClient::send_broadcast_data(const std::vector<uint8_t> &data,
                                    int broadcast_port,
                                    const std::string &broadcast_address_str) {
  boost::system::error_code ec;
  udp::endpoint broadcast_endpoint;

  // handle common broadcast address or specific subnet broadcast
  // todo: make broadcast address logic more robust/configurable
  if (broadcast_address_str == "255.255.255.255" ||
      broadcast_address_str == "10.237.0.255") { // example handling
    broadcast_endpoint =
        udp::endpoint(boost::asio::ip::address_v4::broadcast(), broadcast_port);
  } else {
    boost::asio::ip::address_v4 broadcast_addr =
        boost::asio::ip::make_address_v4(broadcast_address_str, ec);
    if (ec) {
      std::cerr << "[ERROR] UdpClient::send_broadcast_data: Invalid broadcast "
                   "address string '"
                << broadcast_address_str << "': " << ec.message() << std::endl;
      return;
    }
    broadcast_endpoint = udp::endpoint(broadcast_addr, broadcast_port);
  }

  std::cout << "[CLIENT] Sending broadcast (" << data.size() << " bytes) to "
            << broadcast_endpoint << std::endl;

  socket_.async_send_to(
      boost::asio::buffer(data), broadcast_endpoint,
      [this, broadcast_endpoint](const boost::system::error_code &error,
                                 std::size_t /*bytes_sent*/) {
        if (error) {
          std::cerr << "[ERROR] UdpClient::send_broadcast_data failed to "
                    << broadcast_endpoint << ": " << error.message()
                    << std::endl;
        }
      });
}

void UdpClient::set_receive_callback(
    std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
        callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  receive_callback_ = std::move(callback);
}

void UdpClient::start_receive() {
  if (running_.load())
    return;             // use atomic load
  running_.store(true); // use atomic store
  std::cout << "[CLIENT] Starting receiver..." << std::endl;
  do_receive();
}

void UdpClient::stop_receive() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected,
                                        false)) { // stop loop atomically
    return;                                       // already stopped
  }

  if (socket_.is_open()) {
    boost::system::error_code ec;
    socket_.close(ec);
    if (ec) {
      std::cerr << "[ERROR] UdpClient::stop_receive: Error closing socket: "
                << ec.message() << std::endl;
    }
  }
  std::cout << "[CLIENT] Receiver stopped." << std::endl;
}

const udp::endpoint &UdpClient::get_base_endpoint() const {
  if (base_endpoint_.address().is_unspecified()) {
    throw std::runtime_error("Base endpoint not initialized");
  }
  return base_endpoint_;
}

// handles completion of async_receive_from
void UdpClient::handle_receive(const boost::system::error_code &error,
                               std::size_t bytes_transferred) {

  // temp copy of sender endpoint before checking error/running state
  udp::endpoint sender_endpoint = receive_endpoint_;

  if (!error && running_.load()) { // check atomic running flag
    std::vector<uint8_t> received_data(
        receive_buffer_.data(), receive_buffer_.data() + bytes_transferred);

    // get thread-safe copy of callback
    std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
        callback_copy;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback_copy = receive_callback_;
    }

    if (callback_copy) {
      try {
        // call user callback with data and sender endpoint
        callback_copy(received_data, sender_endpoint);
      } catch (const std::exception &e) {
        std::cerr << "[ERROR] UdpClient: Exception in receive callback: "
                  << e.what() << std::endl;
      }
    } else {
      std::cout
          << "[CLIENT] Warning: Received data but no receive callback is set."
          << std::endl;
    }

    // issue next receive operation if still running
    if (running_.load()) {
      do_receive();
    }

  } else if (error == boost::asio::error::operation_aborted) {
    std::cout << "[CLIENT] Receive operation aborted (likely due to stop)."
              << std::endl;
  } else if (running_.load()) { // check running flag again before logging
                                // error/retrying
    std::cerr << "[ERROR] UdpClient receive error: " << error.message()
              << std::endl;

    // retry mechanism on error
    if (running_.load()) { // double check running flag
      std::cerr << "[CLIENT] Attempting to restart receiver after delay ("
                << CLIENT_RETRY_DELAY.count() << "ms)..." << std::endl;
      auto timer = std::make_shared<boost::asio::steady_timer>(
          io_context_, CLIENT_RETRY_DELAY);
      timer->async_wait([this, timer](const boost::system::error_code &ec) {
        // check running flag again inside timer callback
        if (!ec && running_.load()) {
          std::cout << "[CLIENT] Retrying receiver start..." << std::endl;
          do_receive();
        } else if (ec && ec != boost::asio::error::operation_aborted) {
          // log timer error only if not abort
          std::cerr << "[CLIENT] Error waiting for receive retry: "
                    << ec.message() << std::endl;
        }
      });
    }
  }
  // if !running_, do nothing further
}

// initiates one async receive operation
void UdpClient::do_receive() {
  if (!running_.load())
    return; // check running flag

  socket_.async_receive_from(
      boost::asio::buffer(receive_buffer_),
      receive_endpoint_, // capture sender's endpoint here
      [this](const boost::system::error_code &error,
             std::size_t bytes_transferred) {
        handle_receive(error, bytes_transferred);
      });
}