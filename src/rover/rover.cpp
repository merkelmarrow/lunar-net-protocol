// src/rover/rover.cpp
#include "rover.hpp"
#include "command_message.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"
#include <iostream>

Rover::Rover(boost::asio::io_context &io_context, const std::string &base_host,
             int base_port, const std::string &rover_id)
    : io_context_(io_context), status_timer_(io_context),
      session_state_(SessionState::INACTIVE), rover_id_(rover_id),
      current_status_level_(StatusMessage::StatusLevel::OK),
      current_status_description_("System nominal") {
  // initialize UDP client
  client_ = std::make_unique<UdpClient>(io_context);

  // register base station endpoint
  client_->register_base(base_host, base_port);

  // initialize protocol layer
  protocol_ =
      std::make_unique<LumenProtocol>(io_context, *client_, false, true);

  // initialize message manager with client reference
  message_manager_ = std::make_unique<MessageManager>(
      io_context, *protocol_, rover_id, nullptr, client_.get());

  std::cout << "[ROVER] Initialized with ID: " << rover_id
            << ", connecting to base at: " << base_host << ":" << base_port
            << std::endl;
}

Rover::~Rover() { stop(); }

void Rover::start() {
  // start the client
  client_->start_receive();

  // start the protocol layer
  protocol_->start();

  // start the message manager
  message_manager_->start();

  // set up message callback
  message_manager_->set_message_callback(
      [this](std::unique_ptr<Message> message, const udp::endpoint &sender) {
        handle_message(std::move(message), sender);
      });

  // initiate handshake
  initiate_handshake();

  std::cout << "[ROVER] Started and initiating handshake with base station"
            << std::endl;
}

void Rover::stop() {
  // stop status timer
  status_timer_.cancel();

  // stop in reverse order of initialization
  if (message_manager_)
    message_manager_->stop();
  if (protocol_)
    protocol_->stop();
  if (client_)
    client_->stop_receive();

  // update state
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_state_ = SessionState::INACTIVE;
  }

  std::cout << "[ROVER] Stopped" << std::endl;
}

void Rover::send_telemetry(const std::map<std::string, double> &readings) {
  // only send if session is active
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (session_state_ != SessionState::ACTIVE) {
      std::cout << "[ROVER] Cannot send telemetry, session not active"
                << std::endl;
      return;
    }
  }

  TelemetryMessage telemetry_msg(readings, rover_id_);
  message_manager_->send_message(telemetry_msg);

  std::cout << "[ROVER] Sent telemetry data with " << readings.size()
            << " readings" << std::endl;
}

void Rover::update_status(StatusMessage::StatusLevel level,
                          const std::string &description) {
  std::lock_guard<std::mutex> lock(status_mutex_);
  current_status_level_ = level;
  current_status_description_ = description;

  std::cout << "[ROVER] Updated status to: " << static_cast<int>(level) << " - "
            << description << std::endl;
}

Rover::SessionState Rover::get_session_state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return session_state_;
}

void Rover::handle_message(std::unique_ptr<Message> message,
                           const udp::endpoint &sender) {
  std::string msg_type = message->get_type();
  std::string sender_id = message->get_sender();

  std::cout << "[ROVER] Received message type: " << msg_type
            << " from: " << sender_id << std::endl;

  // handle based on message type
  if (msg_type == CommandMessage::message_type()) {
    auto cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    std::string command = cmd_msg->get_command();
    std::string params = cmd_msg->get_params();

    // handle session management commands
    if (command == "SESSION_ACCEPT") {
      handle_session_accept();
    }
    // handle other commands as needed
  }
}

void Rover::send_status() {
  // get current status (thread-safe)
  StatusMessage::StatusLevel level;
  std::string description;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    level = current_status_level_;
    description = current_status_description_;
  }

  // only send if session is active
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (session_state_ != SessionState::ACTIVE) {
      std::cout << "[ROVER] Cannot send status, session not active"
                << std::endl;
      return;
    }
  }

  // create and send status message
  StatusMessage status_msg(level, description, rover_id_);
  message_manager_->send_message(status_msg);

  std::cout << "[ROVER] Sent status: " << static_cast<int>(level) << " - "
            << description << std::endl;
}

void Rover::send_command(const std::string &command,
                         const std::string &params) {
  CommandMessage cmd_msg(command, params, rover_id_);
  message_manager_->send_message(cmd_msg);

  std::cout << "[ROVER] Sent command: " << command << " with params: " << params
            << std::endl;
}

void Rover::initiate_handshake() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_state_ = SessionState::HANDSHAKE_INIT;
  }

  // send SESSION_INIT command
  send_command("SESSION_INIT", rover_id_);

  std::cout << "[ROVER] Sent SESSION_INIT to base station" << std::endl;
}

void Rover::handle_session_accept() {
  std::cout << "[ROVER] Received SESSION_ACCEPT from base station" << std::endl;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // verify we're in the correct state
    if (session_state_ != SessionState::HANDSHAKE_INIT) {
      std::cout
          << "[ROVER] Ignoring SESSION_ACCEPT, not in handshake init state"
          << std::endl;
      return;
    }

    // update state
    session_state_ = SessionState::HANDSHAKE_ACCEPT;
  }

  // send SESSION_CONFIRM
  send_command("SESSION_CONFIRM", rover_id_);

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_state_ = SessionState::ACTIVE;
  }

  std::cout << "[ROVER] Session established with base station" << std::endl;

  // start the status timer to send status every 10 seconds
  handle_status_timer();
}

void Rover::handle_status_timer() {
  // send the status message
  send_status();

  // check if session is still active
  bool is_active;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    is_active = (session_state_ == SessionState::ACTIVE);
  }

  // if active, reschedule timer
  if (is_active) {
    status_timer_.expires_after(std::chrono::seconds(10));
    status_timer_.async_wait([this](const boost::system::error_code &ec) {
      if (!ec) {
        handle_status_timer();
      }
    });
  }
}

void Rover::send_raw_message(const Message &message,
                             const udp::endpoint &endpoint) {
  message_manager_->send_raw_message(message, endpoint);
}