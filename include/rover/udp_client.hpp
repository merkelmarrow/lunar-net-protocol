// include/rover/udp_client.hpp

#pragma once

#include "configs.hpp" // For CLIENT_SOCK_BUF_SIZE, CLIENT_RETRY_DELAY

#include <array>
#include <atomic> // for running_ flag
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/socket_base.hpp> // For broadcast option
#include <boost/system/error_code.hpp>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using boost::asio::ip::udp;

/**
 * @class UdpClient
 * @brief Provides asynchronous UDP client functionality using Boost.Asio.
 *
 * Handles sending unicast and broadcast UDP datagrams and receiving
 * datagrams, providing the sender's endpoint information.
 */
class UdpClient {
public:
  /**
   * @brief Constructor.
   * Creates the UDP socket and binds it to an ephemeral local port.
   * @param io_context The Boost.Asio io_context for asynchronous operations.
   */
  UdpClient(boost::asio::io_context &io_context);

  /**
   * @brief Destructor.
   * Stops receiving and closes the UDP socket.
   */
  ~UdpClient();

  // Prevent copying and assignment
  UdpClient(const UdpClient &) = delete;
  UdpClient &operator=(const UdpClient &) = delete;

  /**
   * @brief Resolves the Base Station's hostname/IP and port, storing the
   * endpoint.
   * @param host The hostname or IP address string of the Base Station.
   * @param port The port number the Base Station is listening on.
   * @throws boost::system::system_error if hostname resolution fails.
   * @throws std::runtime_error if resolution results in no endpoints.
   */
  void register_base(const std::string &host, int port);

  /**
   * @brief Sends data asynchronously to the registered Base Station endpoint.
   * @param data A vector of bytes to send.
   * @throws std::runtime_error if the base endpoint has not been registered.
   */
  void send_data(const std::vector<uint8_t> &data);

  /**
   * @brief Sends data asynchronously to a specific recipient endpoint.
   * @param data A vector of bytes to send.
   * @param recipient The specific UDP endpoint to send the data to.
   */
  void send_data_to(const std::vector<uint8_t> &data,
                    const udp::endpoint &recipient);

  /**
   * @brief Sends data asynchronously as a broadcast message on the local
   * network. Requires the socket to have broadcast option enabled (attempted
   * internally).
   * @param data A vector of bytes to send.
   * @param broadcast_port The port number to broadcast to.
   * @param broadcast_address_str Optional: Specific broadcast address (e.g.,
   * "192.168.1.255"). Defaults to limited broadcast (255.255.255.255).
   */
  void send_broadcast_data(
      const std::vector<uint8_t> &data, int broadcast_port,
      const std::string &broadcast_address_str = "10.237.0.255");

  /**
   * @brief Sets the callback function to be invoked when data is received.
   * @param callback A function object `void(const std::vector<uint8_t>&
   * received_data, const udp::endpoint& sender_endpoint)`.
   */
  void set_receive_callback(
      std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
          callback);

  /**
   * @brief Starts the asynchronous receive loop.
   * Must be called after setting the callback.
   */
  void start_receive();

  /**
   * @brief Stops the asynchronous receive loop and closes the socket.
   */
  void stop_receive();

  /**
   * @brief Gets the resolved endpoint of the registered Base Station.
   * @return const udp::endpoint& A reference to the stored base endpoint.
   * @throws std::runtime_error if called before `register_base`.
   */
  const udp::endpoint &get_base_endpoint() const;

private:
  /**
   * @brief Internal callback handler for completed `async_receive_from`
   * operations.
   * @param error The error code associated with the completed operation.
   * @param bytes_transferred The number of bytes received.
   */
  void handle_receive(const boost::system::error_code &error,
                      std::size_t bytes_transferred);

  /**
   * @brief Initiates a single asynchronous receive operation.
   */
  void do_receive();

  /**
   * @brief Attempts to enable the broadcast socket option. Logs errors.
   */
  void enable_broadcast();

  boost::asio::io_context
      &io_context_;    ///< Reference to the ASIO execution context.
  udp::socket socket_; ///< The UDP socket used for communication.

  udp::endpoint
      base_endpoint_; ///< Resolved endpoint of the target Base Station.
  udp::endpoint receive_endpoint_; ///< Stores the sender's endpoint from the
                                   ///< last received packet.

  std::array<char, CLIENT_SOCK_BUF_SIZE>
      receive_buffer_; ///< Fixed-size buffer for receiving data.

  // MODIFIED Callback Signature
  std::function<void(const std::vector<uint8_t> &, const udp::endpoint &)>
      receive_callback_;

  std::atomic<bool> running_; ///< Flag to control the receive loop.
  std::mutex callback_mutex_; ///< Mutex to protect access to receive_callback_.
};