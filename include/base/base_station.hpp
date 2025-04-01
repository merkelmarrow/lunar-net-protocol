#pragma once

#include "command_message.hpp"
#include "lumen_protocol.hpp"
#include "message.hpp"
#include "message_manager.hpp" // Include MessageManager
#include "status_message.hpp"
#include "telemetry_message.hpp"
#include "udp_server.hpp"
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <functional> // Added for std::function
#include <map>
#include <memory> // Added for std::unique_ptr
#include <mutex>  // Added for std::mutex
#include <string> // Added for std::string

using boost::asio::ip::udp;

class BaseStation {
public:
  enum class SessionState {
    INACTIVE,
    HANDSHAKE_INIT,
    HANDSHAKE_ACCEPT,
    ACTIVE
  };

  // Type alias for the application-level message handler callback
  using ApplicationMessageHandler = std::function<void(
      std::unique_ptr<Message> message, const udp::endpoint &sender)>;

  // Callback specifically for Status/Telemetry (can be kept for convenience)
  using StatusCallback = std::function<void(
      const std::string &rover_id, const std::map<std::string, double> &data)>;

  BaseStation(boost::asio::io_context &io_context, int port,
              const std::string &station_id);
  ~BaseStation();

  void start();
  void stop();

  // Set the callback for application-level messages (non-session management)
  void set_application_message_handler(
      ApplicationMessageHandler handler); // New method

  // Set the callback specifically for Status/Telemetry messages
  void set_status_callback(StatusCallback callback); // Keep this? Optional.

  SessionState get_session_state() const;
  std::string get_connected_rover_id() const;
  udp::endpoint get_rover_endpoint() const; // Added getter

  void send_raw_message(const Message &message, const udp::endpoint &endpoint);
  void send_command(const std::string &command, const std::string &params);

  void send_message(const Message &message, const udp::endpoint &recipient);

private:
  // Internal handler that decides whether to process internally or pass to
  // application handler
  void route_message(std::unique_ptr<Message> message,
                     const udp::endpoint &sender); // Renamed/Refactored

  // Internal handling for specific message types (primarily session management)
  void handle_internal_command(CommandMessage *cmd_msg,
                               const udp::endpoint &sender);
  void handle_internal_status(StatusMessage *status_msg,
                              const udp::endpoint &sender);
  void handle_internal_telemetry(TelemetryMessage *telemetry_msg,
                                 const udp::endpoint &sender);

  // Session management methods (called internally)
  void handle_session_init(const std::string &rover_id,
                           const udp::endpoint &sender);
  void handle_session_confirm(const std::string &rover_id,
                              const udp::endpoint &sender);

  boost::asio::io_context &io_context_;
  std::unique_ptr<UdpServer> server_;
  std::unique_ptr<LumenProtocol> protocol_;
  std::unique_ptr<MessageManager> message_manager_;

  // Application message handler callback
  ApplicationMessageHandler application_message_handler_ =
      nullptr; // New member

  // session state
  SessionState session_state_;
  std::string connected_rover_id_;
  udp::endpoint rover_endpoint_;

  // base station id
  std::string station_id_;

  // callbacks
  StatusCallback status_callback_; // Keep or remove based on preference

  // thread safety
  mutable std::mutex state_mutex_;
  std::mutex callback_mutex_;
  std::mutex handler_mutex_; // Mutex for the application handler
};