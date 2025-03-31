#include "base_station.hpp"

#include "command_message.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"

#include <iostream>

BaseStation::BaseStation(boost::asio::io_context &io_context, int port,
                         const std::string &station_id)
    : io_context_(io_context), session_state_(SessionState::INACTIVE),
      station_id_(station_id) {
  // initialize UDP server
  server_ = std::make_unique<UdpServer>(io_context, port);

  // initialize the protocol layer - base station sends ACKs and expects NAKs
  protocol_ = std::make_unique<LumenProtocol>(io_context, *server_);

  // initialize the message manager with server reference
  message_manager_ = std::make_unique<MessageManager>(
      io_context, *protocol_, station_id, server_.get());

  std::cout << "[BASE STATION] Initialized on port " << port
            << " with ID: " << station_id << std::endl;
}

BaseStation::~BaseStation() { stop(); }

void BaseStation::start() {
  // start the server
  server_->start();

  // start the protocol layer
  protocol_->start();

  // start the message manager
  message_manager_->start();

  // set up message callback
  message_manager_->set_message_callback(
      [this](std::unique_ptr<Message> message, const udp::endpoint &sender) {
        handle_message(std::move(message), sender);
      });

  std::cout << "[BASE STATION] Started and listening for connections"
            << std::endl;
}

void BaseStation::stop() {
  // stop in reverse order of initialization
  if (message_manager_)
    message_manager_->stop();
  if (protocol_)
    protocol_->stop();
  if (server_)
    server_->stop();

  // update state
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_state_ = SessionState::INACTIVE;
    connected_rover_id_.clear();
  }

  std::cout << "[BASE STATION] Stopped" << std::endl;
}

void BaseStation::set_status_callback(StatusCallback callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  status_callback_ = std::move(callback);
}

BaseStation::SessionState BaseStation::get_session_state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return session_state_;
}

std::string BaseStation::get_connected_rover_id() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return connected_rover_id_;
}

void BaseStation::handle_message(std::unique_ptr<Message> message,
                                 const udp::endpoint &sender) {
  std::string msg_type = message->get_type();
  std::string sender_id = message->get_sender();

  std::cout << "[BASE STATION] Received message type: " << msg_type
            << " from: " << sender_id << " at: " << sender << std::endl;

  // handle based on message type
  if (msg_type == CommandMessage::message_type()) {
    auto cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    std::string command = cmd_msg->get_command();
    std::string params = cmd_msg->get_params();

    // handle session management commands
    if (command == "SESSION_INIT") {
      handle_session_init(sender_id, sender);
    } else if (command == "SESSION_CONFIRM") {
      handle_session_confirm(sender_id, sender);
    }
  } else if (msg_type == StatusMessage::message_type()) {
    auto status_msg = dynamic_cast<StatusMessage *>(message.get());

    // only accept status messages from connected rover during active session
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (session_state_ != SessionState::ACTIVE ||
          connected_rover_id_ != sender_id) {
        std::cout
            << "[BASE STATION] Ignoring status message from unconnected rover"
            << std::endl;
        return;
      }
    }

    std::cout << "[BASE STATION] Received status: "
              << static_cast<int>(status_msg->get_level()) << " - "
              << status_msg->get_description() << std::endl;

    // call status callback if set
    StatusCallback callback_copy;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback_copy = status_callback_;
    }

    if (callback_copy) {
      std::map<std::string, double> status_data = {
          {"status_level", static_cast<double>(status_msg->get_level())}};
      callback_copy(sender_id, status_data);
    }
  } else if (msg_type == TelemetryMessage::message_type()) {
    auto telemetry_msg = dynamic_cast<TelemetryMessage *>(message.get());

    // only accept telemetry from connected rover during active session
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (session_state_ != SessionState::ACTIVE ||
          connected_rover_id_ != sender_id) {
        std::cout << "[BASE STATION] Ignoring telemetry from unconnected rover"
                  << std::endl;
        return;
      }
    }

    const auto &readings = telemetry_msg->get_readings();
    std::cout << "[BASE STATION] Received telemetry from " << sender_id
              << std::endl;

    // call status callback if set
    StatusCallback callback_copy;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback_copy = status_callback_;
    }

    if (callback_copy) {
      callback_copy(sender_id, readings);
    }
  }
}

void BaseStation::handle_session_init(const std::string &rover_id,
                                      const udp::endpoint &sender) {
  std::cout << "[BASE STATION] Received SESSION_INIT from rover: " << rover_id
            << std::endl;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // if already in a session, reject the new one
    if (session_state_ == SessionState::ACTIVE &&
        connected_rover_id_ != rover_id) {
      std::cout << "[BASE STATION] Rejecting new session, already connected to "
                << connected_rover_id_ << std::endl;
      return;
    }

    // update state
    session_state_ = SessionState::HANDSHAKE_INIT;
    connected_rover_id_ = rover_id;
    rover_endpoint_ = sender;
  }

  // send SESSION_ACCEPT
  send_command("SESSION_ACCEPT", rover_id);

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_state_ = SessionState::HANDSHAKE_ACCEPT;
  }

  std::cout << "[BASE STATION] Sent SESSION_ACCEPT to rover: " << rover_id
            << std::endl;
}

void BaseStation::handle_session_confirm(const std::string &rover_id,
                                         const udp::endpoint &sender) {
  std::cout << "[BASE STATION] Received SESSION_CONFIRM from rover: "
            << rover_id << std::endl;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // verify this is the rover we're handshaking with
    if (session_state_ != SessionState::HANDSHAKE_ACCEPT ||
        connected_rover_id_ != rover_id) {
      std::cout
          << "[BASE STATION] Ignoring SESSION_CONFIRM from unexpected rover"
          << std::endl;
      return;
    }

    // update state
    session_state_ = SessionState::ACTIVE;
  }

  std::cout << "[BASE STATION] Session established with rover: " << rover_id
            << std::endl;

  // send a session confirmation ACK to let the rover know we're ready
  CommandMessage cmd_msg("SESSION_ESTABLISHED", rover_id, station_id_);
  message_manager_->send_message(cmd_msg, sender);
}

void BaseStation::send_command(const std::string &command,
                               const std::string &params) {
  CommandMessage cmd_msg(command, params, station_id_);

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (rover_endpoint_.address().is_unspecified()) {
      std::cerr << "[BASE STATION] Cannot send command, no rover endpoint"
                << std::endl;
      return;
    }

    message_manager_->send_message(cmd_msg, rover_endpoint_);
  }
}

void BaseStation::send_raw_message(const Message &message,
                                   const udp::endpoint &endpoint) {
  message_manager_->send_raw_message(message, endpoint);
}