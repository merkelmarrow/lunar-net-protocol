// include/rover/rover.hpp
#pragma once

#include "command_message.hpp"
#include "lumen_protocol.hpp"
#include "message_manager.hpp"
#include "status_message.hpp"
#include "udp_client.hpp"
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <functional> // Added for std::function
#include <map>
#include <memory>
#include <mutex>
#include <string>

using boost::asio::ip::udp;

// high-level class to manage rover
class Rover {
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

  Rover(boost::asio::io_context &io_context, const std::string &base_host,
        int base_port, const std::string &rover_id = "grp18-rover");
  ~Rover();

  void start();
  void stop();

  // Set the callback for application-level messages (non-session management)
  void set_application_message_handler(
      ApplicationMessageHandler handler); // New method

  // send telemetry data
  void send_telemetry(const std::map<std::string, double> &readings);

  // update status (will be sent in next status message)
  void update_status(StatusMessage::StatusLevel level,
                     const std::string &description);

  // session state
  SessionState get_session_state() const;

  void send_raw_message(const Message &message, const udp::endpoint &endpoint);

  // send status message (can still be called manually if needed)
  void send_status();

  // send a command message (usually for session management internally, but can
  // be used)
  void send_command(const std::string &command, const std::string &params);

private:
  // Internal handler that decides whether to process internally or pass to
  // application handler
  void route_message(std::unique_ptr<Message> message,
                     const udp::endpoint &sender); // Renamed/Refactored

  // Internal handling for specific message types (primarily session management)
  void handle_internal_command(CommandMessage *cmd_msg,
                               const udp::endpoint &sender);

  // handshake methods
  void initiate_handshake();
  void handle_session_accept();      // Called by handle_internal_command
  void handle_session_established(); // Called by handle_internal_command

  // status timer handler (still useful for periodic status sending)
  void handle_status_timer();

  // Retry handshake if needed
  void handle_handshake_timer();

  // core components
  boost::asio::io_context &io_context_;
  std::unique_ptr<UdpClient> client_;
  std::unique_ptr<LumenProtocol> protocol_;
  std::unique_ptr<MessageManager> message_manager_;

  // status timer
  boost::asio::steady_timer status_timer_;

  // handshake retry timer
  boost::asio::steady_timer handshake_timer_;

  // Application message handler callback
  ApplicationMessageHandler application_message_handler_ =
      nullptr; // New member

  // session state
  SessionState session_state_;

  // rover ID
  std::string rover_id_;

  // current status
  StatusMessage::StatusLevel current_status_level_;
  std::string current_status_description_;

  // thread safety
  mutable std::mutex state_mutex_;
  std::mutex status_mutex_;
  std::mutex handler_mutex_; // Mutex for the application handler

  // handshake retry count
  int handshake_retry_count_;
  static constexpr int MAX_HANDSHAKE_RETRIES = 5;
};