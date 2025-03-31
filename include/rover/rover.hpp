// include/rover/rover.hpp
#pragma once

#include "lumen_protocol.hpp"
#include "message_manager.hpp"
#include "status_message.hpp"
#include "udp_client.hpp"
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
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

  Rover(boost::asio::io_context &io_context, const std::string &base_host,
        int base_port, const std::string &rover_id = "grp18-rover");
  ~Rover();

  void start();
  void stop();

  // send telemetry data
  void send_telemetry(const std::map<std::string, double> &readings);

  // update status (will be sent in next status message)
  void update_status(StatusMessage::StatusLevel level,
                     const std::string &description);

  // session state
  SessionState get_session_state() const;

  void send_raw_message(const Message &message, const udp::endpoint &endpoint);
  // send status message
  void send_status();

  // send a command message
  void send_command(const std::string &command, const std::string &params);

private:
  // handle incoming messages
  void handle_message(std::unique_ptr<Message> message,
                      const udp::endpoint &sender);

  // handshake methods
  void initiate_handshake();
  void handle_session_accept();

  // status timer handler
  void handle_status_timer();

  // core components
  boost::asio::io_context &io_context_;
  std::unique_ptr<UdpClient> client_;
  std::unique_ptr<LumenProtocol> protocol_;
  std::unique_ptr<MessageManager> message_manager_;

  // status timer
  boost::asio::steady_timer status_timer_;

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
};