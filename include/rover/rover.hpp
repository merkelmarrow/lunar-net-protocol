// include/rover/rover.hpp
#pragma once

// Forward declarations for types used in unique_ptr members
class UdpClient;
class LumenProtocol;
class MessageManager;
class Message;
class CommandMessage;
class StatusMessage;
class TelemetryMessage; // Included for update_status parameter type

#include "status_message.hpp" // Need full definition for StatusLevel enum and update_status param
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>     // For timer durations
#include <functional> // For std::function
#include <map>        // For telemetry data map
#include <memory>     // For std::unique_ptr
#include <mutex>      // For std::mutex
#include <string>     // For std::string

using boost::asio::ip::udp;

/**
 * @class Rover
 * @brief Represents the main application logic for the Rover node.
 *
 * Orchestrates the communication layers (UDP Client, Lumen Protocol, Message
 * Manager) for the rover. It handles session management (handshake with the
 * Base Station), manages rover status, sends telemetry and status updates
 * periodically, and routes incoming messages (typically commands) to an
 * application-level handler.
 */
class Rover {
public:
  /**
   * @enum SessionState
   * @brief Defines the possible states of the session with the Base Station.
   */
  enum class SessionState {
    INACTIVE,         ///< No active session, attempting connection or stopped.
    HANDSHAKE_INIT,   ///< Sent SESSION_INIT, waiting for SESSION_ACCEPT.
    HANDSHAKE_ACCEPT, ///< Received SESSION_ACCEPT, sent SESSION_CONFIRM,
                      ///< waiting for SESSION_ESTABLISHED.
    ACTIVE ///< Handshake complete, session is active with the base station.
  };

  /**
   * @typedef ApplicationMessageHandler
   * @brief Callback function type for handling general application messages
   * received from the Base Station. Parameters: unique_ptr<Message> (received
   * message), const udp::endpoint& (sender - should be base).
   */
  using ApplicationMessageHandler =
      std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>;

  /**
   * @brief Constructor. Initializes communication layers and sets target Base
   * Station details.
   * @param io_context The Boost.Asio io_context for all asynchronous
   * operations.
   * @param base_host The hostname or IP address of the Base Station.
   * @param base_port The UDP port the Base Station is listening on.
   * @param rover_id The unique identifier for this Rover. Defaults to
   * "grp18-rover".
   * @throws std::runtime_error if base station hostname resolution fails.
   */
  Rover(boost::asio::io_context &io_context, const std::string &base_host,
        int base_port, const std::string &rover_id = "grp18-rover");

  /**
   * @brief Destructor. Stops the rover and cleans up resources.
   */
  ~Rover();

  // Prevent copying and assignment
  Rover(const Rover &) = delete;
  Rover &operator=(const Rover &) = delete;

  /**
   * @brief Starts the Rover, including all communication layers and initiates
   * the handshake process.
   */
  void start();

  /**
   * @brief Stops the Rover, cancelling timers and shutting down communication
   * layers. Resets session state to INACTIVE.
   */
  void stop();

  /**
   * @brief Registers a callback function to handle general application messages
   * (e.g., commands from Base). This handler receives messages that are not
   * handled internally (like session management).
   * @param handler The function object to call with the deserialized message
   * and sender endpoint.
   */
  void set_application_message_handler(ApplicationMessageHandler handler);

  /**
   * @brief Sends a TelemetryMessage containing the provided readings to the
   * Base Station. Requires the session to be ACTIVE.
   * @param readings A map containing telemetry key-value pairs (string ->
   * double).
   */
  void send_telemetry(const std::map<std::string, double> &readings);

  /**
   * @brief Updates the rover's internal status level and description.
   * This status will be sent in the next periodic status message when the
   * session is ACTIVE.
   * @param level The new status level (OK, WARNING, ERROR, CRITICAL).
   * @param description A string describing the current status.
   */
  void update_status(StatusMessage::StatusLevel level,
                     const std::string &description);

  /**
   * @brief Gets the current session state with the Base Station. Thread-safe.
   * @return SessionState The current state.
   */
  SessionState get_session_state() const;

  /**
   * @brief Sends a standard application message via the full protocol stack to
   * the Base Station. Requires the session to be ACTIVE.
   * @param message The Message object to send.
   */
  void send_message(const Message &message);

  /**
   * @brief Sends a message directly via UDP, bypassing LumenProtocol (sends raw
   * JSON). Requires the MessageManager to have been configured with a UdpClient
   * pointer.
   * @param message The Message object to serialize and send as raw JSON.
   * @param recipient The target UDP endpoint (usually the base station
   * endpoint).
   */
  void send_raw_message(const Message &message, const udp::endpoint &recipient);

  /**
   * @brief Gets the resolved endpoint of the registered Base Station.
   * @return const udp::endpoint& A reference to the stored base endpoint.
   * @throws std::runtime_error if the internal UdpClient is not initialized.
   */
  const udp::endpoint &get_base_endpoint() const;

  // --- Methods primarily for internal use or testing ---

  /**
   * @brief Sends the current status message immediately. Used by the status
   * timer. Requires the session to be ACTIVE.
   */
  void send_status();

  /**
   * @brief Sends a command message to the Base Station.
   * Primarily used internally for session management (INIT, CONFIRM).
   * @param command The command string.
   * @param params Additional parameters string.
   */
  void send_command(const std::string &command, const std::string &params);

private:
  /**
   * @brief Central routing function called by MessageManager when a message is
   * received. Determines if the message should be handled internally (session
   * management) or passed to the general application handler.
   * @param message unique_ptr to the deserialized Message object.
   * @param sender The UDP endpoint of the message sender (should be the base
   * station).
   */
  void route_message(std::unique_ptr<Message> message,
                     const udp::endpoint &sender);

  /**
   * @brief Internal handler for specific command messages received from the
   * Base Station. Currently handles SESSION_ACCEPT and SESSION_ESTABLISHED.
   * @param cmd_msg Pointer to the received CommandMessage.
   * @param sender The sender's endpoint.
   */
  void handle_internal_command(CommandMessage *cmd_msg,
                               const udp::endpoint &sender);

  // --- Session Management Methods ---

  /**
   * @brief Initiates the session handshake by sending SESSION_INIT and starting
   * the retry timer.
   */
  void initiate_handshake();

  /**
   * @brief Handles the SESSION_ACCEPT command received from the Base Station.
   * Transitions state to HANDSHAKE_ACCEPT and sends SESSION_CONFIRM.
   */
  void handle_session_accept();

  /**
   * @brief Handles the SESSION_ESTABLISHED command received from the Base
   * Station. Transitions state to ACTIVE, cancels the handshake timer, and
   * starts the status timer.
   */
  void handle_session_established();

  /**
   * @brief Handler for the handshake retry timer. Resends INIT or CONFIRM if
   * needed.
   */
  void handle_handshake_timer();

  /**
   * @brief Handler for the periodic status timer. Sends the current status and
   * reschedules itself.
   */
  void handle_status_timer();

  // --- Core Components ---
  boost::asio::io_context
      &io_context_; ///< Reference to the ASIO execution context.
  std::unique_ptr<UdpClient> client_;       ///< UDP transport layer client.
  std::unique_ptr<LumenProtocol> protocol_; ///< LUMEN protocol handler.
  std::unique_ptr<MessageManager>
      message_manager_; ///< Message serialization/deserialization manager.

  // --- Timers ---
  boost::asio::steady_timer
      status_timer_; ///< Timer for sending periodic status updates.
  boost::asio::steady_timer
      handshake_timer_; ///< Timer for handling handshake timeouts and retries.

  // --- Callbacks ---
  ApplicationMessageHandler application_message_handler_ =
      nullptr; ///< User-provided handler for app messages.

  // --- State Variables ---
  SessionState
      session_state_; ///< Current state of the session with the base station.
  std::string rover_id_; ///< Unique ID of this Rover.
  StatusMessage::StatusLevel
      current_status_level_; ///< Current reported status level.
  std::string
      current_status_description_; ///< Current reported status description.
  int handshake_retry_count_;      ///< Counter for handshake retry attempts.

  // --- Thread Safety ---
  mutable std::mutex state_mutex_; ///< Protects session_state_.
  std::mutex status_mutex_;        ///< Protects current_status_level_ and
                                   ///< current_status_description_.
  std::mutex handler_mutex_;       ///< Protects application_message_handler_.

  // --- Constants ---
  static constexpr int MAX_HANDSHAKE_RETRIES =
      5; ///< Maximum number of handshake retry attempts.
  static constexpr std::chrono::seconds STATUS_INTERVAL{
      10}; ///< Interval for sending periodic status messages.
  static constexpr std::chrono::seconds HANDSHAKE_TIMEOUT{
      5}; ///< Timeout duration for handshake steps.
};