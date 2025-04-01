// include/rover/udp_client.hpp

#pragma once

#include "configs.hpp" // For CLIENT_SOCK_BUF_SIZE, CLIENT_RETRY_DELAY

#include <array>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/system/error_code.hpp> // Include for error_code
#include <functional>                  // For std::function
#include <mutex>
#include <string> // For host string
#include <vector>

using boost::asio::ip::udp;

/**
 * @class UdpClient
 * @brief Provides asynchronous UDP client functionality using Boost.Asio.
 *
 * This class is designed for the Rover component. It handles sending UDP
 * datagrams to a registered Base Station endpoint and receiving datagrams
 * (presumably from the base). It uses the provided Boost.Asio io_context to
 * manage asynchronous operations.
 */
class UdpClient {
public:
  /**
   * @brief Constructor. Creates the UDP socket and binds it to an ephemeral
   * local port.
   * @param io_context The Boost.Asio io_context that will run the asynchronous
   * operations. Must remain valid for the lifetime of the UdpClient object.
   */
  UdpClient(boost::asio::io_context &io_context);

  /**
   * @brief Destructor. Stops receiving and closes the UDP socket.
   */
  ~UdpClient();

  // Prevent copying and assignment
  UdpClient(const UdpClient &) = delete;
  UdpClient &operator=(const UdpClient &) = delete;

  /**
   * @brief Resolves the Base Station's hostname/IP and port, storing the
   * resulting endpoint. This endpoint is used as the default destination for
   * `send_data`.
   * @param host The hostname or IP address string of the Base Station.
   * @param port The port number the Base Station is listening on.
   * @throws boost::system::system_error if hostname resolution fails.
   * @throws std::runtime_error if resolution results in no endpoints.
   */
  void register_base(const std::string &host, int port);

  /**
   * @brief Sends data asynchronously to the registered Base Station endpoint.
   * The operation executes in the background on the io_context's thread(s).
   * @param data A vector of bytes to send.
   * @throws std::runtime_error if the base endpoint has not been registered via
   * `register_base`.
   */
  void send_data(const std::vector<uint8_t> &data);

  /**
   * @brief Sends data asynchronously to a specific recipient endpoint.
   * This bypasses the registered `base_endpoint_`.
   * @param data A vector of bytes to send.
   * @param recipient The specific UDP endpoint to send the data to.
   */
  void send_data_to(const std::vector<uint8_t> &data,
                    const udp::endpoint &recipient);

  /**
   * @brief Sets the callback function to be invoked when data is received.
   * The callback receives the data as a vector of bytes.
   * @param callback A function object `void(const std::vector<uint8_t>&
   * received_data)`.
   */
  void set_receive_callback(
      std::function<void(const std::vector<uint8_t> &)> callback);

  /**
   * @brief Starts the asynchronous receive loop.
   * Issues the first `async_receive_from` operation. Must be called after
   * setting the callback.
   */
  void start_receive();

  /**
   * @brief Stops the asynchronous receive loop and closes the socket.
   * Any pending receive operations will be cancelled.
   */
  void stop_receive();

  /**
   * @brief Gets the resolved endpoint of the registered Base Station.
   * @return const udp::endpoint& A reference to the stored base endpoint.
   * @throws std::runtime_error (implicitly via send_data) if called before
   * `register_base`.
   */
  const udp::endpoint &get_base_endpoint() const;

private:
  /**
   * @brief Internal callback handler for completed `async_receive_from`
   * operations. Processes received data, invokes the user callback, and
   * potentially initiates the next receive.
   * @param error The error code associated with the completed operation.
   * @param bytes_transferred The number of bytes received.
   */
  void handle_receive(const boost::system::error_code &error,
                      std::size_t bytes_transferred);

  /**
   * @brief Initiates a single asynchronous receive operation using
   * `socket_.async_receive_from`.
   */
  void do_receive();

  boost::asio::io_context
      &io_context_;    ///< Reference to the ASIO execution context.
  udp::socket socket_; ///< The UDP socket used for communication.

  udp::endpoint
      base_endpoint_; ///< Resolved endpoint of the target Base Station.
  udp::endpoint receive_endpoint_; ///< Stores the sender's endpoint from the
                                   ///< last received packet.

  ///< Fixed-size buffer for receiving data via async_receive_from.
  std::array<char, CLIENT_SOCK_BUF_SIZE> receive_buffer_;

  ///< Callback function provided by the user to handle received data.
  std::function<void(const std::vector<uint8_t> &)> receive_callback_;

  std::atomic<bool>
      running_; ///< Flag to control the receive loop. Marked atomic for
                ///< potential cross-thread access visibility (though primarily
                ///< modified/checked within io_context thread).
  std::mutex callback_mutex_; ///< Mutex to protect access to receive_callback_.
};