// include/base/base_station.hpp

#pragma once

class UdpServer;
class LumenProtocol;
class MessageManager;
class Message;
class CommandMessage;
class StatusMessage;
class TelemetryMessage;

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

using boost::asio::ip::udp;

// main application logic for the base station node
class BaseStation {
public:
  // defines the possible states of the session with the rover
  enum class SessionState {
    INACTIVE,
    HANDSHAKE_INIT,   // received session_init
    HANDSHAKE_ACCEPT, // sent session_accept, waiting for session_confirm
    ACTIVE
  };

  // callback function type for handling general, non-internal application
  // messages
  using ApplicationMessageHandler =
      std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>;

  // specific callback function type primarily for handling status and telemetry
  // data
  using StatusCallback = std::function<void(
      const std::string &, const std::map<std::string, double> &)>;

  BaseStation(boost::asio::io_context &io_context, int port,
              const std::string &station_id);

  ~BaseStation();

  void set_low_power_mode(bool enable);
  void set_rover_target(double latitude, double longitude);

  BaseStation(const BaseStation &) = delete;
  BaseStation &operator=(const BaseStation &) = delete;

  void start();

  void stop();

  // registers a callback function to handle general application messages
  void set_application_message_handler(ApplicationMessageHandler handler);

  // registers a specific callback function for status and telemetry messages
  void set_status_callback(StatusCallback callback);

  SessionState get_session_state() const;

  std::string get_connected_rover_id() const;

  udp::endpoint get_rover_endpoint() const;

  // sends a command message to the currently connected rover via the full
  // protocol stack
  void send_command(const std::string &command, const std::string &params);

  // sends an application-level message to a specific recipient via the full
  // protocol stack
  void send_message(const Message &message, const udp::endpoint &recipient);

  // sends a message directly via udp, bypassing lumenprotocol (sends raw json)
  void send_raw_message(const Message &message, const udp::endpoint &recipient);

private:
  // central routing function called by messagemanager when a message is
  // received
  void route_message(std::unique_ptr<Message> message,
                     const udp::endpoint &sender);

  // internal handler for specific command messages (primarily session
  // management)
  void handle_internal_command(CommandMessage *cmd_msg,
                               const udp::endpoint &sender);

  // internal handler that processes statusmessages and invokes the
  // status_callback_
  void handle_internal_status(StatusMessage *status_msg,
                              const StatusCallback &callback);

  // internal handler that processes telemetrymessages and invokes the
  // status_callback_
  void handle_internal_telemetry(TelemetryMessage *telemetry_msg,
                                 const StatusCallback &callback);

  // --- session management methods ---

  // handles the session_init command received from a rover
  void handle_session_init(const std::string &rover_id,
                           const udp::endpoint &sender);

  // handles the session_confirm command received from a rover
  void handle_session_confirm(const std::string &rover_id,
                              const udp::endpoint &sender);

  // --- member variables ---

  boost::asio::io_context &io_context_;
  std::unique_ptr<UdpServer> server_;
  std::unique_ptr<LumenProtocol> protocol_;
  std::unique_ptr<MessageManager> message_manager_;

  ApplicationMessageHandler application_message_handler_ = nullptr;
  StatusCallback status_callback_ = nullptr;

  // session state variables
  SessionState session_state_;
  std::string connected_rover_id_;
  udp::endpoint rover_endpoint_;

  std::string station_id_;

  // mutexes for thread safety
  mutable std::mutex state_mutex_;
  std::mutex callback_mutex_;
  std::mutex handler_mutex_;
};