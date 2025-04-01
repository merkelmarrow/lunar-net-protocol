// include/common/message_manager.hpp

#pragma once

#include "lumen_header.hpp"   // Needs full definition for LumenHeader parameter
#include "lumen_protocol.hpp" // Needs full definition for LumenProtocol reference
#include "message.hpp"        // Needs full definition for Message objects
#include <boost/asio/io_context.hpp> // For io_context reference
#include <boost/asio/ip/udp.hpp>     // For udp::endpoint
#include <cstdint>
#include <functional> // For std::function
#include <memory>     // For std::unique_ptr
#include <mutex>
#include <string> // For sender_id_
#include <vector>

// Forward declare UdpServer/Client if full definition isn't needed in header
class UdpServer;
class UdpClient;

using boost::asio::ip::udp;

/**
 * @class MessageManager
 * @brief Manages the serialization and deserialization of application-level
 * messages.
 *
 * This class acts as a bridge between the application layer (which deals with
 * specific `Message` objects like `CommandMessage`, `StatusMessage`, etc.) and
 * the `LumenProtocol` layer (which deals with binary payloads and protocol
 * headers).
 *
 * Responsibilities:
 * - Serialization: Converts outgoing `Message` objects into JSON strings and
 * then into binary (`std::vector<uint8_t>`) payloads suitable for
 * `LumenProtocol`.
 * - Deserialization: Converts incoming binary payloads (received from
 * `LumenProtocol`) into JSON strings and then uses the `Message::deserialise`
 * factory to create the appropriate `Message` subclass object.
 * - Callback Management: Receives payloads from `LumenProtocol` via a callback
 * and forwards deserialized `Message` objects up to the application layer
 * (e.g., `BaseStation` or `Rover`) via another registered callback.
 * - Raw Message Handling: Optionally allows sending/processing raw JSON
 * messages that bypass the `LumenProtocol` framing, useful for debugging or
 * specific scenarios.
 */
class MessageManager {
public:
  /**
   * @brief Constructor.
   * @param io_context The Boost.Asio io_context (passed down but mainly used by
   * lower layers).
   * @param protocol A reference to the LumenProtocol instance used for
   * sending/receiving framed messages.
   * @param sender_id The unique identifier string for this node (Base Station
   * or Rover ID).
   * @param server Optional pointer to UdpServer (used for raw sends in
   * BaseStation mode).
   * @param client Optional pointer to UdpClient (used for raw sends in Rover
   * mode).
   */
  MessageManager(boost::asio::io_context &io_context, LumenProtocol &protocol,
                 const std::string &sender_id, UdpServer *server = nullptr,
                 UdpClient *client = nullptr);

  /**
   * @brief Destructor. Stops the manager.
   */
  ~MessageManager();

  // Prevent copying and assignment
  MessageManager(const MessageManager &) = delete;
  MessageManager &operator=(const MessageManager &) = delete;

  /**
   * @brief Starts the message manager.
   * Registers necessary callbacks with the lower LumenProtocol layer.
   */
  void start();

  /**
   * @brief Stops the message manager.
   */
  void stop();

  /**
   * @brief Serializes and sends an application-level message via the
   * LumenProtocol layer.
   * @param message The Message object to send (e.g., BasicMessage,
   * CommandMessage).
   * @param recipient The target endpoint. If default/unspecified in Rover mode,
   * sends to the registered base. Required in Base Station mode.
   */
  void send_message(const Message &message,
                    const udp::endpoint &recipient = udp::endpoint());

  /**
   * @brief Sets the callback function to be invoked when a message is
   * successfully deserialized. This callback is typically set by the
   * application layer (BaseStation/Rover) to receive messages.
   * @param callback Function taking (std::unique_ptr<Message> received_message,
   * const udp::endpoint& sender_endpoint).
   */
  void set_message_callback(
      std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
          callback);

  /**
   * @brief Sends a message directly via UDP, bypassing LumenProtocol framing
   * (sends raw JSON). Requires UdpServer or UdpClient pointer to be provided
   * during construction.
   * @param message The Message object to serialize and send as raw JSON.
   * @param recipient The target UDP endpoint.
   */
  void send_raw_message(const Message &message, const udp::endpoint &recipient);

  /**
   * @brief Processes a Message object that was created from raw JSON (received
   * without Lumen headers). Primarily called by LumenProtocol when it detects
   * non-framed JSON data. Forwards the message directly to the application
   * callback.
   * @param message A unique_ptr to the deserialized Message object.
   * @param sender The endpoint from which the raw JSON was received.
   */
  void process_raw_json_message(std::unique_ptr<Message> message,
                                const udp::endpoint &sender);

private:
  /**
   * @brief Internal callback handler called by LumenProtocol upon receiving a
   * framed packet payload. Deserializes the payload and forwards the resulting
   * Message object to the application callback.
   * @param payload The binary payload received from LumenProtocol.
   * @param header The LumenHeader associated with the payload.
   * @param sender The original sender's UDP endpoint.
   */
  void handle_lumen_message(
      const std::vector<uint8_t> &payload,
      const LumenHeader &header, // Header might be useful for context/logging
      const udp::endpoint &sender);

  /**
   * @brief Helper function to convert a string to a vector of bytes.
   * @param str The input string.
   * @return A vector containing the byte representation of the string.
   */
  std::vector<uint8_t> string_to_binary(const std::string &str);

  /**
   * @brief Helper function to convert a vector of bytes to a string.
   * @param data The input vector of bytes.
   * @return A string constructed from the byte data.
   */
  std::string binary_to_string(const std::vector<uint8_t> &data);

  // --- Member Variables ---

  boost::asio::io_context &io_context_; ///< Reference to the ASIO context.
  LumenProtocol
      &protocol_; ///< Reference to the underlying Lumen protocol handler.

  std::string
      sender_id_; ///< Identifier for this node, added to outgoing messages.

  ///< Callback function pointer (set by application) to handle received Message
  ///< objects.
  std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
      message_callback_;

  std::mutex callback_mutex_; ///< Mutex to protect access to message_callback_.

  std::atomic<bool> running_; ///< Flag indicating if the manager is active.

  // Pointers to UDP transport layers (optional, used for raw sends)
  UdpServer *server_; ///< Pointer to UdpServer (if in BaseStation mode and raw
                      ///< send needed).
  UdpClient *
      client_; ///< Pointer to UdpClient (if in Rover mode and raw send needed).
};