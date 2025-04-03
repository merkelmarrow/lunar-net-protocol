// include/rover/rover.hpp
#pragma once

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

// represents the main application logic for the rover node
class Rover {
public:
  // defines the possible states of the session with the base station
  enum class SessionState {
    INACTIVE,
    HANDSHAKE_INIT,   // sent session_init, waiting for session_accept
    HANDSHAKE_ACCEPT, // received session_accept, sent session_confirm, waiting
                      // for session_established
    ACTIVE,
    DISCONNECTED
  };

  using Coordinate = std::pair<double, double>;

  // callback function type for handling general application messages
  using ApplicationMessageHandler =
      std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>;

  // callback function type for handling discovery responses
  using DiscoveryMessageHandler = std::function<void(
      const std::string &rover_id, const udp::endpoint &endpoint)>;

  Rover(boost::asio::io_context &io_context, const std::string &base_host,
        int base_port, const std::string &rover_id = "grp18-rover");

  ~Rover();

  Rover(const Rover &) = delete;
  Rover &operator=(const Rover &) = delete;

  void start();

  void stop();

  // registers a callback function to handle general application messages
  void set_application_message_handler(ApplicationMessageHandler handler);

  // registers a callback function to be notified when a new rover responds
  void set_discovery_message_handler(DiscoveryMessageHandler handler);

  // sends a telemetrymessaage containing the provided readings to the base
  // station
  void send_telemetry(const std::map<std::string, double> &readings);

  // updates the rover's internal status level and description
  void update_status(StatusMessage::StatusLevel level,
                     const std::string &description);

  SessionState get_session_state() const;

  // sends a standard application message via the full protocol stack
  void send_message(const Message &message,
                    const udp::endpoint &recipient = udp::endpoint());

  // sends a message directly via udp, bypassing lumenprotocol (sends raw json)
  void send_raw_message(const Message &message, const udp::endpoint &recipient);

  // initiates a scan for other rovers on the local network
  void scan_for_rovers(int discovery_port,
                       const std::string &message = "ROVER_DISCOVER_PROBE");

  // gets a copy of the currently known discovered rovers
  std::map<std::string, udp::endpoint> get_discovered_rovers() const;

  // gets the resolved endpoint of the registered base station
  const udp::endpoint &get_base_endpoint() const;

  // --- methods primarily for internal use or testing ---

  void send_status();

  // sends a command message to the base station
  void send_command(const std::string &command, const std::string &params);

  void update_current_position(double lat, double lon);
  bool is_low_power_mode();
  void handle_position_telemetry_timer();

private:
  // central routing function called when a message is received
  void route_message(std::unique_ptr<Message> message,
                     const udp::endpoint &sender);

  void handle_base_disconnect();
  void handle_packet_timeout(const udp::endpoint &recipient);
  void send_stored_packets();
  void store_message_for_later(const Message &message,
                               const udp::endpoint &intended_recipient);

  // internal handler for specific command messages received from the base
  // station
  void handle_internal_command(CommandMessage *cmd_msg,
                               const udp::endpoint &sender);

  // internal handler for discovery-related commands
  void handle_discovery_command(CommandMessage *cmd_msg,
                                const udp::endpoint &sender);

  // --- session management methods ---
  void initiate_handshake();
  void handle_session_accept();
  void handle_session_established();

  // --- timer handlers ---
  void handle_handshake_timer();
  void handle_status_timer();
  void handle_probe_timer();
  void handle_movement_timer();

  // --- core components ---
  boost::asio::io_context &io_context_;
  std::unique_ptr<UdpClient> client_;
  std::unique_ptr<LumenProtocol> protocol_;
  std::unique_ptr<MessageManager> message_manager_;

  // --- timers ---
  boost::asio::steady_timer status_timer_;
  boost::asio::steady_timer handshake_timer_;
  boost::asio::steady_timer position_telemetry_timer_;
  boost::asio::steady_timer probe_timer_;
  boost::asio::steady_timer movement_timer_;

  // --- callbacks ---
  ApplicationMessageHandler application_message_handler_ = nullptr;
  DiscoveryMessageHandler discovery_message_handler_ = nullptr;

  // --- state variables ---
  SessionState session_state_;
  std::string rover_id_;
  StatusMessage::StatusLevel current_status_level_;
  std::string current_status_description_;
  int handshake_retry_count_;
  std::map<std::string, udp::endpoint> discovered_rovers_;
  bool low_power_mode_ = false;
  std::optional<Coordinate> target_coordinate_ = std::nullopt;

  // --- thread safety ---
  mutable std::mutex state_mutex_;
  std::mutex status_mutex_;
  std::mutex handler_mutex_;
  mutable std::mutex discovery_mutex_;
  std::mutex discovery_handler_mutex_;
  std::mutex position_mutex_;
  std::mutex target_coord_mutex_;
  std::mutex low_power_mutex_;

  // --- constants ---
  static constexpr int MAX_HANDSHAKE_RETRIES = 5;
  static constexpr std::chrono::seconds STATUS_INTERVAL{15};
  static constexpr std::chrono::seconds HANDSHAKE_TIMEOUT{5};
  static constexpr std::chrono::seconds POSITION_TELEMETRY_INTERVAL{10};

  // position data
  Coordinate current_coordinate_ = {0.0, 0.0};

  // store serialized payload and protocol metadata needed for resending
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