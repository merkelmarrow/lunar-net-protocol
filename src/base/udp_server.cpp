// src/base/udp_server.cpp

#include "udp_server.hpp"
#include "command_message.hpp" // needed for scan_for_rovers
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <iostream>
#include <vector>

UdpServer::UdpServer(boost::asio::io_context &context, int port)
    : io_context_(context), socket_(context, udp::endpoint(udp::v4(), port)),
      running_(false) {
  std::cout << "[SERVER] UDP Server socket created and bound to port " << port
            << std::endl;
}

UdpServer::~UdpServer() { stop(); }

void UdpServer::start() {
  if (running_)
    return;
  running_ = true;
  std::cout << "[SERVER] Starting UDP Server listener..." << std::endl;
  // initiate the first asynchronous receive operation
  receive_data();
}

void UdpServer::stop() {
  running_ = false; // prevent new async operations

  // close socket to interrupt pending operations and release port
  if (socket_.is_open()) {
    boost::system::error_code ec;
    socket_.close(ec);
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

  // initiate an asynchronous send operation
  socket_.async_send_to(boost::asio::buffer(data), recipient,
                        [this, recipient](boost::system::error_code ec,
                                          std::size_t /*bytes_sent*/) {
                          if (ec) {
                            std::cerr
                                << "[ERROR] UdpServer failed to send data to "
                                << recipient << ": " << ec.message()
                                << std::endl;
                          } else {
                            // successful send, optional logging removed for
                            // brevity
                          }
                        });
}

// initiates or continues the asynchronous receive loop
void UdpServer::receive_data() {
  if (!running_)
    return; // don't start new receive if stopped

  // start an asynchronous receive operation
  socket_.async_receive_from(
      boost::asio::buffer(buffer_), // target buffer
      sender_endpoint_,             // populated with sender's endpoint
      // completion handler:
      [this](const boost::system::error_code &ec, std::size_t length) {
        // check if operation successful and server still running
        if (!ec && running_) {
          // convert received data from internal buffer to std::vector
          std::vector<uint8_t> received_data(buffer_.data(),
                                             buffer_.data() + length);

          // get thread-safe copies of endpoint and callback
          udp::endpoint endpoint_copy = sender_endpoint_;
          std::function<void(const std::vector<uint8_t> &,
                             const udp::endpoint &)>
              callback_copy;
          {
            std::lock_guard<std::mutex> callback_lock(callback_mutex_);
            callback_copy = receive_callback_;
          }

          // invoke application-level callback if set
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

          // if still running, issue next receive operation
          if (running_) {
            receive_data(); // recursive call to keep listening
          }
        } else if (ec == boost::asio::error::operation_aborted) {
          // expected when stop() closes socket
          std::cout
              << "[SERVER] Receive operation aborted (likely due to stop)."
              << std::endl;
        } else if (running_) {
          // unexpected error
          std::cerr << "[ERROR] UdpServer receive error: " << ec.message()
                    << std::endl;
          // consider adding retry logic or just stop
          stop(); // simple stop on error for now
        }
        // if !running_, handler returns, stopping the loop
      }); // end of completion handler lambda
}

// provides access to the endpoint of the most recent sender
const udp::endpoint UdpServer::get_sender_endpoint() {
  std::lock_guard<std::mutex> lock(endpoint_mutex_);
  return sender_endpoint_;
}

// used for discovery, sends unicast probes across a subnet range
void UdpServer::scan_for_rovers(int discovery_port, const std::string &message,
                                const std::string &sender_id) {
  if (!running_) {
    std::cerr << "[SERVER SCAN] Error: Server not running." << std::endl;
    return;
  }
  try {
    std::cout << "[SERVER SCAN] Sending unicast scan (" << message
              << ") from sender '" << sender_id
              << "' to subnet 10.237.0.0/24 on port " << discovery_port
              << std::endl;

    CommandMessage discover_msg("ROVER_DISCOVER", message, sender_id);
    std::string json_payload = discover_msg.serialise();
    std::vector<uint8_t> data_to_send(json_payload.begin(), json_payload.end());

    // iterates through a common private ip range, adjust as needed
    for (int i = 2; i <= 120; ++i) { // todo: make range configurable
      std::string ip_str = "10.237.0." + std::to_string(i);
      boost::system::error_code ec;
      boost::asio::ip::address_v4 target_addr =
          boost::asio::ip::make_address_v4(ip_str, ec);
      if (ec) {
        std::cerr << "[SERVER SCAN] Error creating address " << ip_str << ": "
                  << ec.message() << std::endl;
        continue;
      }

      udp::endpoint target_endpoint(
          target_addr, static_cast<unsigned short>(discovery_port));
      send_data(data_to_send, target_endpoint); // uses the async send_data
    }
    std::cout << "[SERVER SCAN] Finished queuing unicast scan packets."
              << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[SERVER SCAN] Error during unicast scan: " << e.what()
              << std::endl;
  }
}