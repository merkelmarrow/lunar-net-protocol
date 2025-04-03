// include/rover/rover.hpp
#pragma once

// Forward declarations
class UdpClient;
class LumenProtocol;
class MessageManager;
class Message;
class CommandMessage;
class StatusMessage;

#include "status_message.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using boost::asio::ip::udp;

/**
 * @class Rover
 * @brief Represents the main application logic for the Rover node.
 *
 * Orchestrates communication layers, handles session management with Base
 * Station, manages rover status, sends telemetry/status updates, supports
 * scanning for other rovers, and routes incoming messages to application
 * handlers.
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
    ACTIVE, ///< Handshake complete, session is active with the base station.
    DISCONNECTED
  };

  using Coordinate = std::pair<double, double>;

  /**
   * @typedef ApplicationMessageHandler
   * @brief Callback function type for handling general application messages
   * received from the Base Station or other Rovers.
   * Parameters: unique_ptr<Message> (received message), const udp::endpoint&
   * (sender).
   */
  using ApplicationMessageHandler =
      std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>;

  /**
   * @typedef DiscoveryMessageHandler
   * @brief Callback function type for handling discovery responses when a new
   * rover is found. Parameters: const std::string& (rover_id), const
   * udp::endpoint& (endpoint).
   */
  using DiscoveryMessageHandler = std::function<void(
      const std::string &rover_id, const udp::endpoint &endpoint)>;

  /**
   * @brief Constructor.
   * Initializes communication layers and sets target Base Station details.
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
   * the handshake process with the base station.
   */
  void start();

  /**
   * @brief Stops the Rover, cancelling timers and shutting down communication
   * layers. Resets session state and clears discovered rovers.
   */
  void stop();

  /**
   * @brief Registers a callback function to handle general application messages
   * (e.g., commands from Base or other Rovers).
   * @param handler The function object to call with the deserialized message
   * and sender endpoint.
   */
  void set_application_message_handler(ApplicationMessageHandler handler);

  /**
   * @brief Registers a callback function to be notified when a new rover
   * responds to a discovery scan.
   * @param handler Function taking (rover_id, rover_endpoint).
   */
  void set_discovery_message_handler(DiscoveryMessageHandler handler);

  /**
   * @brief Sends a TelemetryMessage containing the provided readings to the
   * Base Station (if session is ACTIVE).
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
   * @brief Sends a standard application message via the full protocol stack.
   * If recipient is unspecified and session is ACTIVE, sends to Base Station.
   * Otherwise, sends to the specified recipient endpoint.
   * @param message The Message object to send.
   * @param recipient Optional target endpoint (e.g., base station or a
   * discovered rover).
   */
  void send_message(const Message &message,
                    const udp::endpoint &recipient = udp::endpoint());

  /**
   * @brief Sends a message directly via UDP, bypassing LumenProtocol (sends raw
   * JSON). Requires the MessageManager to have been configured with a UdpClient
   * pointer.
   * @param message The Message object to serialize and send as raw JSON.
   * @param recipient The target UDP endpoint (e.g., base station or a
   * discovered rover).
   */
  void send_raw_message(const Message &message, const udp::endpoint &recipient);

  /**
   * @brief Initiates a scan for other rovers on the local network.
   * Sends a broadcast discovery message. Responses update the internal map and
   * trigger callback.
   * @param discovery_port The port number rovers listen on for discovery
   * messages.
   * @param message Optional custom discovery message content (defaults to basic
   * probe command).
   */
  void scan_for_rovers(int discovery_port,
                       const std::string &message = "ROVER_DISCOVER_PROBE");

  /**
   * @brief Gets a copy of the currently known discovered rovers. Thread-safe.
   * @return std::map<std::string, udp::endpoint> Map of rover IDs to their
   * endpoints.
   */
  std::map<std::string, udp::endpoint> get_discovered_rovers() const;

  /**
   * @brief Gets the resolved endpoint of the registered Base Station.
   * @return const udp::endpoint& A reference to the stored base endpoint.
   * @throws std::runtime_error if the internal UdpClient is not initialized or
   * registration failed.
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

  void update_current_position(double lat, double lon);
  bool is_low_power_mode();
  void handle_position_telemetry_timer();

private:
  /**
   * @brief Central routing function called when a message is received.
   * Determines if the message should be handled internally (session, discovery)
   * or passed to the general application handler. Differentiates sender.
   * @param message unique_ptr to the deserialized Message object.
   * @param sender The UDP endpoint of the message sender.
   */
  void route_message(std::unique_ptr<Message> message,
                     const udp::endpoint &sender);

  void handle_base_disconnect();
  void handle_packet_timeout(const udp::endpoint &recipient);
  void send_stored_packets();
  void store_message_for_later(const Message &message,
                               const udp::endpoint &intended_recipient);

  /**
   * @brief Internal handler for specific command messages received from the
   * Base Station (SESSION_ACCEPT, SESSION_ESTABLISHED).
   * @param cmd_msg Pointer to the received CommandMessage.
   * @param sender The sender's endpoint (must be base station).
   */
  void handle_internal_command(CommandMessage *cmd_msg,
                               const udp::endpoint &sender);

  /**
   * @brief Internal handler for discovery-related commands (e.g.,
   * ROVER_ANNOUNCE response, ROVER_DISCOVER probe). Updates discovered_rovers_
   * map and triggers callback/response.
   * @param cmd_msg Pointer to the received CommandMessage.
   * @param sender The sender's endpoint.
   */
  void handle_discovery_command(CommandMessage *cmd_msg,
                                const udp::endpoint &sender);

  // --- Session Management Methods ---
  void initiate_handshake();
  void handle_session_accept();
  void handle_session_established();

  // --- Timer Handlers ---
  void handle_handshake_timer();
  void handle_status_timer();
  void handle_probe_timer();
  void handle_movement_timer();

  // --- Core Components ---
  boost::asio::io_context &io_context_;
  std::unique_ptr<UdpClient> client_;
  std::unique_ptr<LumenProtocol> protocol_;
  std::unique_ptr<MessageManager> message_manager_;

  // --- Timers ---
  boost::asio::steady_timer status_timer_;
  boost::asio::steady_timer handshake_timer_;
  boost::asio::steady_timer position_telemetry_timer_;
  boost::asio::steady_timer probe_timer_;
  boost::asio::steady_timer movement_timer_;

  // --- Callbacks ---
  ApplicationMessageHandler application_message_handler_ = nullptr;
  DiscoveryMessageHandler discovery_message_handler_ = nullptr;

  // --- State Variables ---
  SessionState session_state_;
  std::string rover_id_;
  StatusMessage::StatusLevel current_status_level_;
  std::string current_status_description_;
  int handshake_retry_count_;
  std::map<std::string, udp::endpoint> discovered_rovers_;
  bool low_power_mode_ = false;
  std::optional<Coordinate> target_coordinate_ = std::nullopt;

  // --- Thread Safety ---
  mutable std::mutex state_mutex_;
  std::mutex status_mutex_;
  std::mutex handler_mutex_;
  mutable std::mutex discovery_mutex_;
  std::mutex discovery_handler_mutex_;
  std::mutex position_mutex_;
  std::mutex target_coord_mutex_;
  std::mutex low_power_mutex_;

  // --- Constants ---
  static constexpr int MAX_HANDSHAKE_RETRIES = 5;
  static constexpr std::chrono::seconds STATUS_INTERVAL{15};
  static constexpr std::chrono::seconds HANDSHAKE_TIMEOUT{5};
  static constexpr std::chrono::seconds POSITION_TELEMETRY_INTERVAL{10};

  // position data
  Coordinate current_coordinate_ = {0.0, 0.0};

  // Store serialized payload and protocol metadata needed for resending
  using StoredPacketData =
      std::tuple<std::vector<uint8_t>, LumenHeader::MessageType,
                 LumenHeader::Priority, udp::endpoint>;
  std::deque<StoredPacketData> stored_packets_;
  std::mutex stored_packets_mutex_;

  static constexpr double MOVE_RATE_METERS_PER_SECOND = 1.0;
  static constexpr std::chrono::milliseconds MOVEMENT_INTERVAL{1000};
  static constexpr double TARGET_REACHED_THRESHOLD_METERS = 1.0;

  static constexpr std::chrono::seconds PROBE_INTERVAL{5};
};