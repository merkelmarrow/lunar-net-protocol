// src/base/udp_server.cpp

#include "udp_server.hpp"
#include "configs.hpp" // For SERVER_SOCK_BUF_SIZE
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <iostream>
#include <memory> // For std::make_shared
#include <vector> // For std::vector

UdpServer::UdpServer(boost::asio::io_context &context, int port)
    : // Initialize the socket, binding it to the specified port on all
      // available IPv4 interfaces.
      socket_(context, udp::endpoint(udp::v4(), port)), running_(false) {
  std::cout << "[SERVER] UDP Server socket created and bound to port " << port
            << std::endl;
}

UdpServer::~UdpServer() { stop(); }

void UdpServer::start() {
  if (running_)
    return;
  running_ = true;
  std::cout << "[SERVER] Starting UDP Server listener..." << std::endl;
  // Initiate the first asynchronous receive operation.
  receive_data();
}

void UdpServer::stop() {
  running_ = false; // Prevent new async operations from being started

  // Close the socket to interrupt pending operations and release the port.
  if (socket_.is_open()) {
    boost::system::error_code ec;
    socket_.close(ec); // Close the socket
    if (ec) {
      std::cerr << "[ERROR] Error closing server socket: " << ec.message()
                << std::endl;
    } else {
      std::cout << "[SERVER] UDP Server socket closed." << std::endl;
    }
  }
  std::cout << "[SERVER] UDP Server stopped." << std::endl;
}

void UdpServer::set_receive_callback(
    std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
        callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  receive_callback_ = std::move(callback);
}

void UdpServer::send_data(const std::vector<uint8_t> &data,
                          const udp::endpoint &recipient) {
  if (!running_) {
    std::cerr << "[ERROR] UdpServer::send_data: Server not running."
              << std::endl;
    return;
  }
  if (recipient.address().is_unspecified() || recipient.port() == 0) {
    std::cerr << "[ERROR] UdpServer::send_data: Invalid recipient endpoint."
              << std::endl;
    return;
  }

  // Initiate an asynchronous send operation.
  socket_.async_send_to(boost::asio::buffer(data), recipient,
                        // Completion handler lambda:
                        [this, recipient](boost::system::error_code ec,
                                          std::size_t bytes_sent /*length*/) {
                          if (ec) {
                            std::cerr
                                << "[ERROR] UdpServer failed to send data to "
                                << recipient << ": " << ec.message()
                                << std::endl;
                          } else {
                            // Optional: Log successful send - can be verbose.
                            // std::cout << "[SERVER] Sent " << bytes_sent << "
                            // bytes to " << recipient << std::endl;
                          }
                        });
}

// Initiates or continues the asynchronous receive loop.
void UdpServer::receive_data() {
  if (!running_)
    return; // Don't start a new receive if stopped

  // Start an asynchronous receive operation.
  // Data will be placed into buffer_.
  // The sender's endpoint will be stored in sender_endpoint_.
  // The provided lambda is the completion handler.
  socket_.async_receive_from(
      boost::asio::buffer(buffer_), // Target buffer
      sender_endpoint_,             // Will be populated with sender's endpoint
      // Completion Handler:
      [this](const boost::system::error_code &ec, std::size_t length) {
        // Check if the operation was successful and if the server is still
        // running.
        if (!ec && running_) {
          // Convert the received data from the internal buffer to a
          // std::vector.
          std::vector<uint8_t> received_data(buffer_.data(),
                                             buffer_.data() + length);

          // std::cout << "[SERVER] Received " << length << " bytes from " <<
          // sender_endpoint_ << std::endl; // Reduced verbosity

          // Get thread-safe copies of the endpoint and callback.
          // sender_endpoint_ is updated by async_receive_from before this
          // handler runs.
          udp::endpoint endpoint_copy = sender_endpoint_;
          std::function<void(const std::vector<uint8_t> &,
                             const udp::endpoint &)>
              callback_copy;
          {
            // Note: Locking endpoint_mutex might not be strictly necessary if
            // only accessed here and get_sender_endpoint, but kept for
            // consistency if access patterns change. Callback mutex is
            // important. std::lock_guard<std::mutex>
            // endpoint_lock(endpoint_mutex_); // Might be overkill
            std::lock_guard<std::mutex> callback_lock(callback_mutex_);
            // endpoint_copy = sender_endpoint_; // Already captured by value
            // above
            callback_copy = receive_callback_;
          }

          // Invoke the application-level callback if it's set.
          if (callback_copy) {
            try {
              callback_copy(received_data, endpoint_copy);
            } catch (const std::exception &e) {
              std::cerr << "[ERROR] UdpServer: Exception in receive callback: "
                        << e.what() << std::endl;
            }
          } else {
            std::cout << "[SERVER] Warning: Received data but no receive "
                         "callback is set."
                      << std::endl;
          }

          // If still running, issue the next receive operation to continue
          // listening.
          if (running_) {
            receive_data(); // Recursive call to keep listening
          }
        } else if (ec == boost::asio::error::operation_aborted) {
          // Operation aborted is expected when stop() closes the socket.
          std::cout
              << "[SERVER] Receive operation aborted (likely due to stop)."
              << std::endl;
        } else if (running_) {
          // An unexpected error occurred.
          std::cerr << "[ERROR] UdpServer receive error: " << ec.message()
                    << std::endl;
          // Consider if the server should stop or try to recover.
          // For now, we stop the receive loop on error. To retry, a mechanism
          // similar to UdpClient's timer-based retry could be added here.
          // stop(); // Example: Stop server on receive error
          // Or: Attempt to restart receive after a delay
          if (running_) { // Check running_ again
            std::cerr << "[SERVER] Attempting to restart receiver..."
                      << std::endl;
            // Re-issue receive immediately (might loop on persistent errors)
            // or use a timer for delayed retry. Let's try immediate for now.
            receive_data();
          }
        }
        // If !running_, the handler simply returns, stopping the loop.
      }); // End of completion handler lambda
}

// Provides access to the endpoint of the most recent sender. Note potential
// race condition if called exactly when a new packet arrives and
// sender_endpoint_ is being updated. Locking helps.
const udp::endpoint UdpServer::get_sender_endpoint() {
  std::lock_guard<std::mutex> lock(endpoint_mutex_);
  return sender_endpoint_;
}