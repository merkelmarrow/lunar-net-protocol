// include/base/udp_server.hpp

#pragma once

#include "configs.hpp" // For SERVER_SOCK_BUF_SIZE
#include <array>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <functional> // For std::function
#include <memory>     // For std::shared_ptr in potential complex handlers
#include <mutex>
#include <vector>

using boost::asio::ip::udp;

/**
 * @class UdpServer
 * @brief Provides asynchronous UDP server functionality using Boost.Asio.
 *
 * This class is designed for the Base Station component. It binds to a
 * specified port, listens for incoming UDP datagrams, and provides mechanisms
 * to send datagrams to specific client endpoints. It uses the provided
 * Boost.Asio io_context to manage asynchronous operations.
 */
class UdpServer {
public:
  /**
   * @brief Constructor. Creates and binds the UDP socket to the specified port.
   * @param context The Boost.Asio io_context that will run the asynchronous
   * operations. Must remain valid for the lifetime of the UdpServer object.
   * @param port The UDP port number to listen on.
   * @throws boost::system::system_error if the socket cannot be created or
   * bound to the port.
   */
  UdpServer(boost::asio::io_context &context, int port);

  /**
   * @brief Destructor. Stops listening and closes the UDP socket.
   */
  ~UdpServer();

  // Prevent copying and assignment
  UdpServer(const UdpServer &) = delete;
  UdpServer &operator=(const UdpServer &) = delete;

  /**
   * @brief Starts the server's asynchronous receive loop.
   * Begins listening for incoming UDP datagrams. Requires
   * `set_receive_callback` to be called first.
   */
  void start();

  /**
   * @brief Stops the server's receive loop and closes the socket.
   * Any pending receive operations will be cancelled.
   */
  void stop();

  /**
   * @brief Sets the callback function to be invoked when a UDP datagram is
   * received. The callback receives the data as a vector of bytes and the
   * sender's endpoint.
   * @param callback A function object `void(const std::vector<uint8_t>&
   * received_data, const udp::endpoint& sender_endpoint)`.
   */
  void set_receive_callback(
      std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
          callback);

  /**
   * @brief Sends data asynchronously to a specific recipient endpoint.
   * @param data A vector of bytes to send.
   * @param recipient The specific UDP endpoint to send the data to.
   */
  void send_data(const std::vector<uint8_t> &data,
                 const udp::endpoint &recipient);

  /**
   * @brief Gets the UDP endpoint of the sender of the most recently received
   * datagram. Note: This value is updated asynchronously. Use with appropriate
   * synchronization if needed.
   * @return const udp::endpoint The endpoint of the last known sender. May be
   * default-constructed if no data received yet.
   */
  const udp::endpoint get_sender_endpoint();

private:
  /**
   * @brief Initiates a single asynchronous receive operation.
   * This function is called recursively by the completion handler to maintain
   * the receive loop.
   */
  void receive_data();

  udp::socket socket_; ///< The UDP socket used for listening and sending.
  udp::endpoint sender_endpoint_; ///< Stores the endpoint of the client from
                                  ///< the last successful receive.

  ///< Fixed-size buffer for receiving data via async_receive_from.
  std::array<char, SERVER_SOCK_BUF_SIZE> buffer_;

  ///< Callback function provided by the user (e.g., LumenProtocol) to handle
  ///< received data.
  std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
      receive_callback_;

  std::mutex endpoint_mutex_; ///< Mutex to protect access to sender_endpoint_
                              ///< (primarily for get_sender_endpoint).
  std::mutex callback_mutex_; ///< Mutex to protect access to receive_callback_.
  std::atomic<bool> running_; ///< Flag to control the receive loop. Marked
                              ///< atomic for visibility.
};