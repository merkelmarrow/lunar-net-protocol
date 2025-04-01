// src/rover/udp_client.cpp

#include "udp_client.hpp" // Header for this class
#include "configs.hpp"    // For CLIENT_RETRY_DELAY, CLIENT_SOCK_BUF_SIZE

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp> // For retry delay
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp> // For throwing exceptions
#include <iostream>
#include <memory> // For std::make_shared in retry timer
#include <mutex>
#include <stdexcept> // For std::runtime_error
#include <vector>

UdpClient::UdpClient(boost::asio::io_context &io_context)
    : io_context_(io_context),
      // Initialize the socket, binding to any available local UDP port on IPv4
      socket_(io_context, udp::endpoint(udp::v4(), 0)), running_(false) {}

UdpClient::~UdpClient() { stop_receive(); }

void UdpClient::register_base(const std::string &host, int port) {
  try {
    udp::resolver resolver(io_context_);
    // Resolve the hostname/IP and port into one or more endpoint objects.
    // This handles potential DNS lookups.
    auto endpoints = resolver.resolve(udp::v4(), host, std::to_string(port));

    if (endpoints.empty()) {
      throw std::runtime_error("Could not resolve host: " + host);
    }
    // Store the first resolved endpoint as the target for send_data().
    base_endpoint_ = *endpoints.begin();

    std::cout << "[CLIENT] Registered base station endpoint: " << base_endpoint_
              << std::endl;

  } catch (const boost::system::system_error &e) {
    std::cerr << "[ERROR] Failed to resolve or register base endpoint (" << host
              << ":" << port << "): " << e.what() << std::endl;
    throw; // Re-throw as the client likely cannot function without a valid base
           // endpoint.
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Unexpected error during base registration: "
              << e.what() << std::endl;
    throw;
  }
}

// Sends data asynchronously to the registered base_endpoint_.
void UdpClient::send_data(const std::vector<uint8_t> &data) {
  if (base_endpoint_.address().is_unspecified()) {
    std::cerr << "[ERROR] UdpClient::send_data: Base endpoint is not "
                 "initialized. Call register_base() first."
              << std::endl;
    throw std::runtime_error("Base endpoint not initialized for UdpClient");
  }

  // Use async_send_to for non-blocking send.
  // The provided lambda will be called upon completion (or error).
  socket_.async_send_to(
      boost::asio::buffer(data), base_endpoint_,
      // Completion handler lambda:
      [this](const boost::system::error_code &error, std::size_t bytes_sent) {
        if (error) {
          std::cerr << "[ERROR] UdpClient::send_data failed: "
                    << error.message() << std::endl;
          // Consider adding more robust error handling/reporting here if
          // needed.
        } else {
          // Optional: Log successful send - can be verbose.
          // std::cout << "[CLIENT] Sent " << bytes_sent << " bytes to base " <<
          // base_endpoint_ << std::endl;
        }
      });
}

// Sends data asynchronously to a specific recipient endpoint, ignoring the
// registered base_endpoint_.
void UdpClient::send_data_to(const std::vector<uint8_t> &data,
                             const udp::endpoint &recipient) {
  if (recipient.address().is_unspecified() || recipient.port() == 0) {
    std::cerr << "[ERROR] UdpClient::send_data_to: Invalid recipient endpoint "
                 "provided."
              << std::endl;
    return; // Or throw? For now, just log and return.
  }

  socket_.async_send_to(
      boost::asio::buffer(data), recipient,
      // Completion handler lambda:
      [this, recipient](const boost::system::error_code &error,
                        std::size_t bytes_sent) {
        if (error) {
          std::cerr << "[ERROR] UdpClient::send_data_to failed for "
                    << recipient << ": " << error.message() << std::endl;
        } else {
          // Optional: Log successful send.
          // std::cout << "[CLIENT] Sent " << bytes_sent << " bytes to specific
          // endpoint " << recipient << std::endl;
        }
      });
}

void UdpClient::set_receive_callback(
    std::function<void(const std::vector<uint8_t> &)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  receive_callback_ = std::move(callback);
}

void UdpClient::start_receive() {
  if (running_)
    return;
  running_ = true;
  std::cout << "[CLIENT] Starting receiver..." << std::endl;
  do_receive(); // Initiate the first asynchronous receive operation.
}

void UdpClient::stop_receive() {
  running_ = false; // Signal handlers to stop initiating new operations

  // Close the socket to interrupt any pending blocking operations and free
  // resources.
  if (socket_.is_open()) {
    boost::system::error_code ec;
    socket_.close(ec); // Close the socket
    if (ec) {
      // Log error during close, but it's often non-critical during shutdown.
      std::cerr << "[ERROR] UdpClient::stop_receive: Error closing socket: "
                << ec.message() << std::endl;
    }
  }
  std::cout << "[CLIENT] Receiver stopped." << std::endl;
}

const udp::endpoint &UdpClient::get_base_endpoint() const {
  return base_endpoint_;
}

// Private handler called by Boost.Asio when an async_receive_from operation
// completes.
void UdpClient::handle_receive(const boost::system::error_code &error,
                               std::size_t bytes_transferred) {
  // Check if the operation was successful and if we are still supposed to be
  // running.
  if (!error && running_) {
    // Convert the received data from the internal buffer to a std::vector.
    std::vector<uint8_t> received_data(
        receive_buffer_.data(), receive_buffer_.data() + bytes_transferred);

    // std::cout << "[CLIENT] Received " << bytes_transferred << " bytes from "
    // << receive_endpoint_ << std::endl; // Reduced verbosity

    // Get a thread-safe copy of the callback function pointer.
    std::function<void(const std::vector<uint8_t> &)> callback_copy;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback_copy = receive_callback_;
    }

    // If a callback is registered, invoke it with the received data.
    if (callback_copy) {
      try {
        callback_copy(received_data);
      } catch (const std::exception &e) {
        std::cerr << "[ERROR] UdpClient: Exception in receive callback: "
                  << e.what() << std::endl;
        // Decide if error is fatal or if receiving should continue.
      }

    } else {
      std::cout
          << "[CLIENT] Warning: Received data but no receive callback is set."
          << std::endl;
    }

    // If still running, immediately issue the next asynchronous receive
    // operation.
    if (running_) {
      do_receive();
    }

  } else if (error == boost::asio::error::operation_aborted) {
    // Operation aborted is expected when stop_receive() closes the socket.
    std::cout << "[CLIENT] Receive operation aborted (likely due to stop)."
              << std::endl;
  } else if (running_) {
    // An unexpected error occurred during receive.
    std::cerr << "[ERROR] UdpClient receive error: " << error.message()
              << std::endl;
    // Optional: Implement a retry mechanism with delay instead of stopping
    // immediately. Example: Use a timer to retry starting receive after a
    // delay.
    if (running_) { // Check running_ again as it might have changed
      std::cerr << "[CLIENT] Attempting to restart receiver after delay ("
                << CLIENT_RETRY_DELAY.count() << "ms)..." << std::endl;
      auto timer = std::make_shared<boost::asio::steady_timer>(
          io_context_, CLIENT_RETRY_DELAY);
      timer->async_wait([this, timer](const boost::system::error_code &ec) {
        if (!ec && running_) {
          std::cout << "[CLIENT] Retrying receiver start..." << std::endl;
          do_receive(); // Try initiating receive again
        } else if (ec) {
          std::cerr << "[CLIENT] Error waiting for receive retry: "
                    << ec.message() << std::endl;
        }
      });
    }
  }
  // If !running_, do nothing further.
}

// Initiates an asynchronous receive operation.
void UdpClient::do_receive() {
  if (!running_)
    return; // Don't start new operation if stopped

  // Start an asynchronous receive. The socket will wait for data.
  // The data will be placed into receive_buffer_.
  // receive_endpoint_ will be populated with the sender's address and port.
  // The provided lambda (which calls handle_receive) will be executed on
  // completion.
  socket_.async_receive_from(
      boost::asio::buffer(receive_buffer_), // Target buffer for received data
      receive_endpoint_, // Endpoint variable to store sender info
      // Completion handler:
      [this](const boost::system::error_code &error,
             std::size_t bytes_transferred) {
        handle_receive(error, bytes_transferred);
      });
}