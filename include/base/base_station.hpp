// include/base/base_station.hpp

#pragma once

// Forward declarations for types used in unique_ptr members to minimize header
// includes
class UdpServer;
class LumenProtocol;
class MessageManager;
class Message;
class CommandMessage;
class StatusMessage;
class TelemetryMessage;

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <functional> // For std::function
#include <map>        // For StatusCallback map type
#include <memory>     // For std::unique_ptr
#include <mutex>      // For std::mutex
#include <string>     // For std::string

using boost::asio::ip::udp;

/**
 * @class BaseStation
 * @brief Represents the main application logic for the Base Station node.
 *
 * This class orchestrates the communication layers (UDP Server, Lumen Protocol,
 * Message Manager) for the base station. It handles session management
 * (handshake with a single Rover), routes incoming messages to appropriate
 * internal handlers or application-level callbacks, and provides methods for
 * sending commands and messages to the connected Rover.
 */
class BaseStation {
public:
  /**
   * @enum SessionState
   * @brief Defines the possible states of the session with a Rover.
   */
  enum class SessionState {
    INACTIVE,         ///< No active session, listening for connections.
    HANDSHAKE_INIT,   ///< Received SESSION_INIT from a rover. DEPRECATED
                      ///< (transition happens within handler now).
    HANDSHAKE_ACCEPT, ///< Sent SESSION_ACCEPT, waiting for SESSION_CONFIRM.
    ACTIVE ///< Handshake complete, session is active with a connected rover.
  };

  /**
   * @typedef ApplicationMessageHandler
   * @brief Callback function type for handling general, non-internal
   * application messages. Parameters: unique_ptr<Message> (received message),
   * const udp::endpoint& (sender).
   */
  using ApplicationMessageHandler =
      std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>;

  /**
   * @typedef StatusCallback
   * @brief Specific callback function type primarily for handling Status and
   * Telemetry data. Parameters: const std::string& (rover_id), const
   * std::map<std::string, double>& (data map).
   */
  using StatusCallback = std::function<void(
      const std::string &, const std::map<std::string, double> &)>;

  /**
   * @brief Constructor. Initializes communication layers.
   * @param io_context The Boost.Asio io_context for all asynchronous
   * operations.
   * @param port The UDP port number the Base Station should listen on.
   * @param station_id The unique identifier for this Base Station.
   */
  BaseStation(boost::asio::io_context &io_context, int port,
              const std::string &station_id);

  /**
   * @brief Destructor. Stops the station and cleans up resources.
   */
  ~BaseStation();

  // Prevent copying and assignment
  BaseStation(const BaseStation &) = delete;
  BaseStation &operator=(const BaseStation &) = delete;

  /**
   * @brief Starts the Base Station, including all underlying communication
   * layers. Begins listening for incoming connections/messages.
   */
  void start();

  /**
   * @brief Stops the Base Station and all underlying communication layers.
   * Resets session state.
   */
  void stop();

  /**
   * @brief Registers a callback function to handle general application
   * messages. This handler receives messages that are not handled internally
   * (like session management) and potentially messages not handled by the
   * specific `StatusCallback`.
   * @param handler The function object to call with the deserialized message
   * and sender endpoint.
   */
  void set_application_message_handler(ApplicationMessageHandler handler);

  /**
   * @brief Registers a specific callback function for Status and Telemetry
   * messages. If set, Status and Telemetry messages may be routed here instead
   * of (or in addition to) the general application message handler.
   * @param callback The function object to call with the rover ID and data map.
   */
  void set_status_callback(StatusCallback callback);

  /**
   * @brief Gets the current session state with the Rover. Thread-safe.
   * @return SessionState The current state.
   */
  SessionState get_session_state() const;

  /**
   * @brief Gets the ID of the currently connected Rover (if session is active).
   * Thread-safe.
   * @return std::string The Rover ID, or an empty string if no active session.
   */
  std::string get_connected_rover_id() const;

  /**
   * @brief Gets the UDP endpoint of the currently connected Rover (if session
   * is active or during handshake). Thread-safe.
   * @return udp::endpoint The Rover's endpoint, or a default-constructed
   * endpoint if unknown.
   */
  udp::endpoint get_rover_endpoint() const;

  /**
   * @brief Sends a command message to the currently connected Rover via the
   * full protocol stack. Requires the session to be ACTIVE (or HANDSHAKE_ACCEPT
   * for session responses).
   * @param command The command string (e.g., "MOVE_FORWARD").
   * @param params Additional parameters for the command, as a string.
   */
  void send_command(const std::string &command, const std::string &params);

  /**
   * @brief Sends an application-level message to a specific recipient via the
   * full protocol stack. Useful for sending messages when the recipient might
   * not be the currently connected rover, or when explicitly targeting an
   * endpoint.
   * @param message The Message object to send.
   * @param recipient The target UDP endpoint. Must be valid.
   */
  void send_message(const Message &message, const udp::endpoint &recipient);

  /**
   * @brief Sends a message directly via UDP, bypassing LumenProtocol (sends raw
   * JSON). Requires the MessageManager to have been configured with a UdpServer
   * pointer.
   * @param message The Message object to serialize and send as raw JSON.
   * @param recipient The target UDP endpoint.
   */
  void send_raw_message(const Message &message, const udp::endpoint &recipient);

private:
  /**
   * @brief Central routing function called by MessageManager when a message is
   * received. Determines if the message should be handled internally (session,
   * specific callbacks) or passed to the general application handler. Performs
   * session validation.
   * @param message unique_ptr to the deserialized Message object.
   * @param sender The UDP endpoint of the message sender.
   */
  void route_message(std::unique_ptr<Message> message,
                     const udp::endpoint &sender);

  /**
   * @brief Internal handler for specific command messages (primarily session
   * management).
   * @param cmd_msg Pointer to the received CommandMessage.
   * @param sender The sender's endpoint.
   */
  void handle_internal_command(CommandMessage *cmd_msg,
                               const udp::endpoint &sender);

  /**
   * @brief Internal handler that processes StatusMessages and invokes the
   * `status_callback_` (if set).
   * @param status_msg Pointer to the received StatusMessage.
   * @param callback A copy of the status_callback_ function object.
   */
  void handle_internal_status(StatusMessage *status_msg,
                              const StatusCallback &callback);

  /**
   * @brief Internal handler that processes TelemetryMessages and invokes the
   * `status_callback_` (if set).
   * @param telemetry_msg Pointer to the received TelemetryMessage.
   * @param callback A copy of the status_callback_ function object.
   */
  void handle_internal_telemetry(TelemetryMessage *telemetry_msg,
                                 const StatusCallback &callback);

  // --- Session Management Methods ---

  /**
   * @brief Handles the SESSION_INIT command received from a Rover.
   * Manages state transitions and sends SESSION_ACCEPT.
   * @param rover_id The ID of the rover initiating the session.
   * @param sender The UDP endpoint of the initiating rover.
   */
  void handle_session_init(const std::string &rover_id,
                           const udp::endpoint &sender);

  /**
   * @brief Handles the SESSION_CONFIRM command received from a Rover.
   * Verifies the sender and state, transitions to ACTIVE, and sends
   * SESSION_ESTABLISHED.
   * @param rover_id The ID of the rover confirming the session.
   * @param sender The UDP endpoint of the confirming rover.
   */
  void handle_session_confirm(const std::string &rover_id,
                              const udp::endpoint &sender);

  // --- Member Variables ---

  boost::asio::io_context
      &io_context_; ///< Reference to the ASIO execution context.
  std::unique_ptr<UdpServer> server_;       ///< UDP transport layer server.
  std::unique_ptr<LumenProtocol> protocol_; ///< LUMEN protocol handler.
  std::unique_ptr<MessageManager>
      message_manager_; ///< Message serialization/deserialization manager.

  ///< Callback for general application messages.
  ApplicationMessageHandler application_message_handler_ = nullptr;
  ///< Specific callback for status/telemetry data.
  StatusCallback status_callback_ = nullptr;

  // Session State Variables
  SessionState session_state_; ///< Current state of the session.
  std::string
      connected_rover_id_; ///< ID of the currently connected/handshaking rover.
  udp::endpoint
      rover_endpoint_; ///< UDP endpoint of the connected/handshaking rover.

  std::string station_id_; ///< Unique ID of this Base Station.

  // Mutexes for thread safety
  mutable std::mutex state_mutex_; ///< Protects session_state_,
                                   ///< connected_rover_id_, rover_endpoint_.
  std::mutex callback_mutex_;      ///< Protects status_callback_.
  std::mutex handler_mutex_;       ///< Protects application_message_handler_.
};