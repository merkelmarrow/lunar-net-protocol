// src/rover/rover.cpp
#include "rover.hpp"
#include "command_message.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"
#include <iostream>

Rover::Rover(boost::asio::io_context &io_context, const std::string &base_host,
             int base_port, const std::string &rover_id)
    : io_context_(io_context), status_timer_(io_context),
      handshake_timer_(io_context), session_state_(SessionState::INACTIVE),
      rover_id_(rover_id),
      current_status_level_(StatusMessage::StatusLevel::OK),
      current_status_description_("System nominal"), handshake_retry_count_(0) {
  // initialize UDP client
  client_ = std::make_unique<UdpClient>(io_context);

  // register base station endpoint
  client_->register_base(base_host, base_port);

  // initialize protocol layer - rover sends NAKs (not ACKs) and expects ACKs
  protocol_ = std::make_unique<LumenProtocol>(io_context, *client_);

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

  // Set up message callback to route messages
  message_manager_->set_message_callback(
      [this](std::unique_ptr<Message> message, const udp::endpoint &sender) {
        route_message(std::move(message), sender); // Use the router function
      });

  // initiate handshake
  initiate_handshake();

  std::cout << "[ROVER] Started and initiating handshake with base station"
            << std::endl;
}

void Rover::stop() {
  // stop status timer
  status_timer_.cancel();

  // stop handshake timer
  handshake_timer_.cancel();

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

// New method to set the application-level message handler
void Rover::set_application_message_handler(ApplicationMessageHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  application_message_handler_ = std::move(handler);
  std::cout << "[ROVER] Application message handler set." << std::endl;
}

void Rover::send_telemetry(const std::map<std::string, double> &readings) {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (session_state_ != SessionState::ACTIVE) {
      std::cout << "[ROVER] Cannot send telemetry, session not active (current "
                   "state: "
                << static_cast<int>(session_state_) << ")" << std::endl;
      return;
    }
  }

  TelemetryMessage telemetry_msg(readings, rover_id_);
  message_manager_->send_message(telemetry_msg);

  // Removed logging from here, can be added in main if needed
  // std::cout << "[ROVER] Sent telemetry data with " << readings.size()
  //           << " readings" << std::endl;
}

void Rover::update_status(StatusMessage::StatusLevel level,
                          const std::string &description) {
  std::lock_guard<std::mutex> lock(status_mutex_);
  current_status_level_ = level;
  current_status_description_ = description;

  std::cout << "[ROVER] Internal status updated to: " << static_cast<int>(level)
            << " - " << description << std::endl;
}

Rover::SessionState Rover::get_session_state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return session_state_;
}

// Renamed/Refactored: Routes message to internal handler or application handler
void Rover::route_message(std::unique_ptr<Message> message,
                          const udp::endpoint &sender) {
  if (!message)
    return;

  std::string msg_type = message->get_type();
  std::string sender_id = message->get_sender();

  std::cout << "[ROVER INTERNAL] Routing message type: " << msg_type
            << " from: " << sender_id << std::endl;

  // Check if it's a command message that needs internal handling
  if (msg_type == CommandMessage::message_type()) {
    auto cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    if (cmd_msg) {
      std::string command = cmd_msg->get_command();
      // Session management commands MUST be handled internally
      if (command == "SESSION_ACCEPT" || command == "SESSION_ESTABLISHED") {
        handle_internal_command(cmd_msg, sender);
        return; // Don't pass session commands to application handler
      }
    }
  }

  // If not handled internally, pass to the application handler if it exists
  ApplicationMessageHandler handler_copy;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler_copy = application_message_handler_;
  }

  if (handler_copy) {
    std::cout << "[ROVER INTERNAL] Passing message type " << msg_type
              << " to application handler." << std::endl;
    // Use post to avoid blocking the network thread if the handler is slow
    boost::asio::post(io_context_,
                      [handler = std::move(handler_copy),
                       msg = std::move(message),
                       ep = sender]() mutable { handler(std::move(msg), ep); });
  } else {
    std::cout
        << "[ROVER INTERNAL] No application handler set for message type: "
        << msg_type << std::endl;
  }
}

// Handles only essential internal commands (like session management)
void Rover::handle_internal_command(CommandMessage *cmd_msg,
                                    const udp::endpoint &sender) {
  if (!cmd_msg)
    return;

  std::string command = cmd_msg->get_command();
  std::string params = cmd_msg->get_params();

  std::cout << "[ROVER INTERNAL] Handling command: " << command << std::endl;

  if (command == "SESSION_ACCEPT") {
    handle_session_accept();
  } else if (command == "SESSION_ESTABLISHED") {
    handle_session_established();
  }
  // Other internal commands could be added here if needed
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
      // std::cout << "[ROVER] Cannot send status, session not active" <<
      // std::endl; // Reduce noise
      return;
    }
  }

  // create and send status message
  StatusMessage status_msg(level, description, rover_id_);
  message_manager_->send_message(status_msg);

  // Removed logging from here, can be added in main if needed
  // std::cout << "[ROVER] Sent status: " << static_cast<int>(level) << " - "
  //           << description << std::endl;
}

void Rover::send_command(const std::string &command,
                         const std::string &params) {
  CommandMessage cmd_msg(command, params, rover_id_);
  message_manager_->send_message(cmd_msg);

  // Keep essential logging for commands sent
  std::cout << "[ROVER INTERNAL] Sent command: " << command
            << " with params: " << params << std::endl;
}

void Rover::initiate_handshake() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_state_ = SessionState::HANDSHAKE_INIT;
  }

  // send SESSION_INIT command
  send_command("SESSION_INIT", rover_id_);

  std::cout << "[ROVER INTERNAL] Sent SESSION_INIT to base station"
            << std::endl;

  // Start a timer to retry handshake if no response
  handshake_timer_.expires_after(std::chrono::seconds(5));
  handshake_timer_.async_wait([this](const boost::system::error_code &ec) {
    if (!ec) {
      handle_handshake_timer();
    }
  });
}

void Rover::handle_handshake_timer() {
  SessionState current_state;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  // If we're still in HANDSHAKE_INIT or HANDSHAKE_ACCEPT state, retry
  if (current_state == SessionState::HANDSHAKE_INIT ||
      current_state == SessionState::HANDSHAKE_ACCEPT) {

    if (handshake_retry_count_ < MAX_HANDSHAKE_RETRIES) {
      handshake_retry_count_++;
      std::cout << "[ROVER INTERNAL] Handshake timeout, retrying (attempt "
                << handshake_retry_count_ << "/" << MAX_HANDSHAKE_RETRIES << ")"
                << std::endl;

      if (current_state == SessionState::HANDSHAKE_INIT) {
        // Retry sending SESSION_INIT
        send_command("SESSION_INIT", rover_id_);
      } else if (current_state == SessionState::HANDSHAKE_ACCEPT) {
        // Retry sending SESSION_CONFIRM
        send_command("SESSION_CONFIRM", rover_id_);
      }

      // Schedule next retry
      handshake_timer_.expires_after(std::chrono::seconds(5));
      handshake_timer_.async_wait([this](const boost::system::error_code &ec) {
        if (!ec) {
          handle_handshake_timer();
        }
      });
    } else {
      std::cout << "[ROVER INTERNAL] Handshake failed after "
                << MAX_HANDSHAKE_RETRIES << " attempts. Giving up."
                << std::endl;

      // Reset state to INACTIVE
      std::lock_guard<std::mutex> lock(state_mutex_);
      session_state_ = SessionState::INACTIVE;
    }
  }
}

// Called internally when SESSION_ACCEPT command is received
// Called internally when SESSION_ACCEPT command is received
void Rover::handle_session_accept() {
  std::cout << "[ROVER INTERNAL] Processing SESSION_ACCEPT" << std::endl; //

  bool state_updated = false; // Flag to track if state changed

  {                                                 // Lock scope starts
    std::lock_guard<std::mutex> lock(state_mutex_); // Lock acquired

    // verify we're in the correct state
    if (session_state_ != SessionState::HANDSHAKE_INIT) { //
      std::cout << "[ROVER INTERNAL] Ignoring SESSION_ACCEPT, not in handshake "
                   "init state"
                << std::endl; //
      return;                 // Exit early if state is wrong
    }

    // update state
    session_state_ = SessionState::HANDSHAKE_ACCEPT; //
    state_updated = true;                            // Mark state as updated
  } // Lock released here

  // Only proceed if the state was correctly updated
  if (state_updated) {
    // send SESSION_CONFIRM
    send_command("SESSION_CONFIRM", rover_id_); //

    // ** FIX: Cancel the handshake timer now **
    // We've received ACCEPT and sent CONFIRM, no need for the timer to retry.
    handshake_timer_.cancel(); //
    std::cout << "[ROVER INTERNAL] Handshake timer cancelled after sending "
                 "SESSION_CONFIRM."
              << std::endl; // Optional log
  }
  // NOTE: Do NOT set state to ACTIVE here. Wait for SESSION_ESTABLISHED.
  // NOTE: Do NOT start status_timer_ here. Wait for session to be ACTIVE.
}

// Called internally when SESSION_ESTABLISHED command is received
void Rover::handle_session_established() {
  std::cout << "[ROVER INTERNAL] Processing SESSION_ESTABLISHED"
            << std::endl; //
  bool needs_status_timer_start =
      false; // Flag to start timer after lock release

  {                                                 // Lock scope starts
    std::lock_guard<std::mutex> lock(state_mutex_); // Lock acquired
    if (session_state_ !=
        SessionState::ACTIVE) { // Only transition if not already active
      if (session_state_ != SessionState::HANDSHAKE_ACCEPT) { //
        std::cout << "[ROVER INTERNAL] Warning: Received SESSION_ESTABLISHED "
                     "in unexpected state: "
                  << static_cast<int>(session_state_) << std::endl; //
      }
      session_state_ = SessionState::ACTIVE;                       //
      std::cout << "[ROVER INTERNAL] Session ACTIVE" << std::endl; //

      // Reset retry counter now that session is fully confirmed
      handshake_retry_count_ = 0; //

      // Cancel handshake timer as it's no longer needed
      handshake_timer_.cancel(); //

      // Set the flag to start the status timer *after* releasing the lock
      needs_status_timer_start = true; //
    }
  } // Lock released here

  // Start the status timer ONLY when active and *after* the lock is released
  if (needs_status_timer_start) { //
    handle_status_timer();        // <<-- MOVED OUTSIDE LOCK SCOPE
  }
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
      // Only reschedule if the timer wasn't cancelled and rover is still active
      if (!ec && get_session_state() == SessionState::ACTIVE) {
        handle_status_timer();
      }
    });
  } else {
    std::cout << "[ROVER INTERNAL] Status timer stopped (session inactive)."
              << std::endl;
  }
}

void Rover::send_raw_message(const Message &message,
                             const udp::endpoint &endpoint) {
  message_manager_->send_raw_message(message, endpoint);
}