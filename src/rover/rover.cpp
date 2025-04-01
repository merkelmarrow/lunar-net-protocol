// src/rover/rover.cpp

#include "rover.hpp"
#include "command_message.hpp" // For sending/receiving commands
#include "message_manager.hpp"
#include "status_message.hpp"    // For sending status
#include "telemetry_message.hpp" // For sending telemetry
#include <boost/asio/post.hpp>   // For posting handler execution
#include <chrono>                // For timer durations
#include <exception>             // For std::exception
#include <iostream>
#include <memory> // For std::unique_ptr, std::move

Rover::Rover(boost::asio::io_context &io_context, const std::string &base_host,
             int base_port, const std::string &rover_id)
    : io_context_(io_context), status_timer_(io_context),
      handshake_timer_(io_context),
      session_state_(SessionState::INACTIVE), // Initialize as inactive
      rover_id_(rover_id),
      current_status_level_(StatusMessage::StatusLevel::OK), // Default status
      current_status_description_("System nominal"),         // Default status
      handshake_retry_count_(0) {

  // Initialize communication layers: Transport -> Protocol -> Message Manager
  client_ = std::make_unique<UdpClient>(io_context);
  try {
    client_->register_base(base_host,
                           base_port); // Resolve base station endpoint
  } catch (const std::exception &e) {
    // Log the error and re-throw; Rover cannot operate without a valid base
    // endpoint.
    std::cerr << "[ROVER] CRITICAL ERROR during initialization: Failed to "
                 "register base station at "
              << base_host << ":" << base_port << ". Error: " << e.what()
              << std::endl;
    throw; // Propagate the error up
  }
  protocol_ =
      std::make_unique<LumenProtocol>(io_context, *client_); // Pass client ref
  message_manager_ = std::make_unique<MessageManager>(
      io_context, *protocol_, rover_id_, nullptr,
      client_.get()); // Pass protocol, id, client ref

  std::cout << "[ROVER] Initialized with ID: '" << rover_id
            << "', target base: " << base_host << ":" << base_port << std::endl;
}

Rover::~Rover() {
  stop(); // Ensure timers are cancelled and resources released
}

void Rover::start() {
  if (message_manager_)
    message_manager_->start();
  if (protocol_)
    protocol_->start();
  if (client_)
    client_->start_receive(); // Start receiving UDP data last

  // Set up the callback chain: MessageManager receives deserialized messages
  // and passes them up to Rover's route_message method.
  if (message_manager_) {
    message_manager_->set_message_callback([this](
                                               std::unique_ptr<Message> message,
                                               const udp::endpoint &sender) {
      // We expect messages only from the base station endpoint in normal
      // operation
      if (client_ && sender == client_->get_base_endpoint()) {
        route_message(std::move(message), sender);
      } else {
        std::cerr
            << "[ROVER] Warning: Ignoring message from unexpected endpoint: "
            << sender << std::endl;
      }
    });
  } else {
    std::cerr
        << "[ROVER] Error: Cannot set message callback, MessageManager is null."
        << std::endl;
  }

  // Initiate the handshake process with the base station.
  initiate_handshake();

  std::cout << "[ROVER] Started and initiating handshake." << std::endl;
}

void Rover::stop() {
  // Cancel timers first to prevent rescheduling
  boost::system::error_code ec; // To ignore errors on cancel
  status_timer_.cancel(ec);
  handshake_timer_.cancel(ec);

  // Stop layers in reverse order of initialization/start
  if (client_)
    client_->stop_receive();
  if (protocol_)
    protocol_->stop();
  if (message_manager_)
    message_manager_->stop();

  // Reset session state
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_state_ = SessionState::INACTIVE;
  }

  std::cout << "[ROVER] Stopped" << std::endl;
}

// Set the handler for general application messages (non-internal)
void Rover::set_application_message_handler(ApplicationMessageHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  application_message_handler_ = std::move(handler);
  std::cout << "[ROVER] Application message handler registered." << std::endl;
}

// Sends telemetry data if the session is active.
void Rover::send_telemetry(const std::map<std::string, double> &readings) {
  bool can_send = false;
  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);
    can_send = (session_state_ == SessionState::ACTIVE);
  } // State Mutex Lock Released

  if (can_send) {
    TelemetryMessage telemetry_msg(readings, rover_id_);
    if (message_manager_) {
      message_manager_->send_message(
          telemetry_msg); // Sends to default base endpoint
      // std::cout << "[ROVER] Sent telemetry data (" << readings.size() << "
      // readings)." << std::endl; // Reduced verbosity
    } else {
      std::cerr
          << "[ROVER] Error: Cannot send telemetry, MessageManager is null."
          << std::endl;
    }
  } else {
    // std::cout << "[ROVER] Cannot send telemetry, session not active." <<
    // std::endl; // Reduce noise
  }
}

// Updates the internal status variables. Does not send immediately.
void Rover::update_status(StatusMessage::StatusLevel level,
                          const std::string &description) {
  std::lock_guard<std::mutex> lock(status_mutex_);
  current_status_level_ = level;
  current_status_description_ = description;
  // std::cout << "[ROVER] Internal status updated to: " <<
  // static_cast<int>(level) << " - " << description << std::endl; // Reduce
  // verbosity
}

Rover::SessionState Rover::get_session_state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return session_state_;
}

// Central router for messages received from MessageManager
void Rover::route_message(std::unique_ptr<Message> message,
                          const udp::endpoint &sender) {
  if (!message) {
    std::cerr
        << "[ROVER] Error: Received null message pointer in route_message."
        << std::endl;
    return;
  }

  std::string msg_type = message->get_type();
  std::string sender_id =
      message->get_sender(); // ID from within the message payload

  // std::cout << "[ROVER INTERNAL] Routing message type: '" << msg_type << "'
  // from sender ID: '" << sender_id << "'" << std::endl; // Reduced verbosity

  // --- Internal Handling ---
  // Handle session management commands specifically.
  if (msg_type == CommandMessage::message_type()) {
    auto *cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    if (cmd_msg) {
      const std::string &command = cmd_msg->get_command();
      // SESSION_ACCEPT and SESSION_ESTABLISHED are handled internally.
      if (command == "SESSION_ACCEPT" || command == "SESSION_ESTABLISHED") {
        handle_internal_command(cmd_msg, sender);
        // Session commands are fully handled internally, do not pass further.
        return;
      }
      // Potentially handle other internal commands here (e.g., "REBOOT",
      // "SET_PARAM")
    }
  }

  // --- Application Handling ---
  // If not handled internally, pass to the registered application handler.
  ApplicationMessageHandler handler_copy;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler_copy = application_message_handler_;
  }

  if (handler_copy) {
    // std::cout << "[ROVER INTERNAL] Passing message type '" << msg_type << "'
    // to application handler." << std::endl; // Reduced verbosity Post handler
    // execution to io_context to avoid blocking network thread.
    boost::asio::post(io_context_, [handler = std::move(handler_copy),
                                    msg = std::move(message),
                                    ep = sender]() mutable {
      try {
        handler(std::move(msg), ep);
      } catch (const std::exception &e) {
        std::cerr << "[ERROR] Rover: Exception in application message handler: "
                  << e.what() << std::endl;
      }
    });
  } else {
    std::cout << "[ROVER INTERNAL] No application handler registered for "
                 "message type: '"
              << msg_type << "' from sender ID '" << sender_id << "'."
              << std::endl;
  }
}

// Handles internal command messages (currently only session-related)
void Rover::handle_internal_command(CommandMessage *cmd_msg,
                                    const udp::endpoint &sender) {
  if (!cmd_msg)
    return;

  const std::string &command = cmd_msg->get_command();
  // std::string params = cmd_msg->get_params(); // Params might contain Base
  // Station ID

  // std::cout << "[ROVER INTERNAL] Handling internal command: '" << command <<
  // "'" << std::endl; // Reduced verbosity

  if (command == "SESSION_ACCEPT") {
    handle_session_accept();
  } else if (command == "SESSION_ESTABLISHED") {
    handle_session_established();
  }
  // Add handlers for other internal commands if needed
}

// Sends the current status message if the session is active.
void Rover::send_status() {
  StatusMessage::StatusLevel current_level;
  std::string current_desc;
  bool can_send = false;

  { // Status Mutex Lock Scope
    std::lock_guard<std::mutex> lock(status_mutex_);
    current_level = current_status_level_;
    current_desc = current_status_description_;
  } // Status Mutex Lock Released

  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);
    can_send = (session_state_ == SessionState::ACTIVE);
  } // State Mutex Lock Released

  if (can_send) {
    StatusMessage status_msg(current_level, current_desc, rover_id_);
    if (message_manager_) {
      message_manager_->send_message(status_msg);
      // std::cout << "[ROVER] Sent status: " << static_cast<int>(current_level)
      // << " - " << current_desc << std::endl; // Reduced verbosity
    } else {
      std::cerr << "[ROVER] Error: Cannot send status, MessageManager is null."
                << std::endl;
    }
  } else {
    // Status timer might call this when inactive, just ignore.
    // std::cout << "[ROVER] Cannot send status, session not active." <<
    // std::endl;
  }
}

// Sends a command message (typically for session management).
void Rover::send_command(const std::string &command,
                         const std::string &params) {
  if (!message_manager_) {
    std::cerr << "[ROVER] Error: Cannot send command '" << command
              << "', MessageManager is null." << std::endl;
    return;
  }
  CommandMessage cmd_msg(command, params, rover_id_);
  message_manager_->send_message(cmd_msg); // Sends to default base endpoint
  // std::cout << "[ROVER INTERNAL] Sent command: '" << command << "' with
  // params: '" << params << "'" << std::endl; // Reduced verbosity
}

// --- Session Management Implementation ---

// Starts the handshake process by sending SESSION_INIT.
void Rover::initiate_handshake() {
  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Only initiate if inactive to avoid interrupting existing
    // sessions/handshakes.
    if (session_state_ != SessionState::INACTIVE) {
      std::cout << "[ROVER INTERNAL] Handshake initiation skipped, state is "
                   "not INACTIVE ("
                << static_cast<int>(session_state_) << ")" << std::endl;
      return;
    }
    session_state_ = SessionState::HANDSHAKE_INIT;
    handshake_retry_count_ = 0; // Reset retry count at the start
  } // State Mutex Lock Released

  // Send the initial SESSION_INIT command.
  send_command("SESSION_INIT", rover_id_); // Params contain rover's ID
  std::cout << "[ROVER INTERNAL] Sent SESSION_INIT." << std::endl;

  // Start the handshake timer to handle potential timeouts.
  boost::system::error_code ec;
  handshake_timer_.expires_after(HANDSHAKE_TIMEOUT);
  handshake_timer_.async_wait([this](const boost::system::error_code &ec) {
    if (!ec) { // If the timer wasn't cancelled
      handle_handshake_timer();
    }
  });
}

// Called by the handshake timer if no response is received within the timeout.
void Rover::handle_handshake_timer() {
  SessionState current_state;
  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  } // State Mutex Lock Released

  // Check if handshake is still pending (INIT or ACCEPT state)
  if (current_state == SessionState::HANDSHAKE_INIT ||
      current_state == SessionState::HANDSHAKE_ACCEPT) {
    if (handshake_retry_count_ < MAX_HANDSHAKE_RETRIES) {
      handshake_retry_count_++;
      std::cout << "[ROVER INTERNAL] Handshake timeout. Retrying (attempt "
                << handshake_retry_count_ << "/" << MAX_HANDSHAKE_RETRIES
                << ")..." << std::endl;

      // Resend the appropriate command based on the current state.
      if (current_state == SessionState::HANDSHAKE_INIT) {
        send_command("SESSION_INIT", rover_id_);
      } else { // HANDSHAKE_ACCEPT
        send_command("SESSION_CONFIRM", rover_id_);
      }

      // Reschedule the timer for the next retry attempt.
      boost::system::error_code ec;
      handshake_timer_.expires_after(HANDSHAKE_TIMEOUT);
      handshake_timer_.async_wait([this](const boost::system::error_code &ec) {
        if (!ec) {
          handle_handshake_timer();
        }
      });
    } else {
      // Max retries reached, give up.
      std::cerr << "[ROVER INTERNAL] Handshake failed after "
                << MAX_HANDSHAKE_RETRIES << " attempts. Resetting to INACTIVE."
                << std::endl;
      { // State Mutex Lock Scope
        std::lock_guard<std::mutex> lock(state_mutex_);
        session_state_ = SessionState::INACTIVE;
      } // State Mutex Lock Released
    }
  }
  // If state is INACTIVE or ACTIVE, the timer callback does nothing further.
}

// Handles receiving SESSION_ACCEPT from the base station.
void Rover::handle_session_accept() {
  std::cout << "[ROVER INTERNAL] Received SESSION_ACCEPT." << std::endl;
  bool state_updated = false;

  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Only proceed if we were expecting this (in HANDSHAKE_INIT state).
    if (session_state_ == SessionState::HANDSHAKE_INIT) {
      session_state_ = SessionState::HANDSHAKE_ACCEPT;
      state_updated = true;
      std::cout << "[ROVER INTERNAL] State updated to HANDSHAKE_ACCEPT."
                << std::endl;
    } else {
      std::cout << "[ROVER INTERNAL] Ignoring SESSION_ACCEPT received in "
                   "unexpected state: "
                << static_cast<int>(session_state_) << std::endl;
    }
  } // State Mutex Lock Released

  if (state_updated) {
    // Send SESSION_CONFIRM back to the base station.
    send_command("SESSION_CONFIRM", rover_id_);
    std::cout << "[ROVER INTERNAL] Sent SESSION_CONFIRM." << std::endl;

    // Handshake timer is still running; wait for SESSION_ESTABLISHED or
    // timeout. Do not cancel the timer here.
  }
  // Do not start the status timer yet.
}

// Handles receiving SESSION_ESTABLISHED from the base station.
void Rover::handle_session_established() {
  std::cout << "[ROVER INTERNAL] Received SESSION_ESTABLISHED." << std::endl;
  bool needs_status_timer_start = false;

  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Transition to ACTIVE state, ideally from HANDSHAKE_ACCEPT.
    if (session_state_ !=
        SessionState::ACTIVE) { // Prevent multiple starts of status timer if
                                // already active
      if (session_state_ != SessionState::HANDSHAKE_ACCEPT) {
        // Log if received in an unexpected state, but still transition.
        std::cout << "[ROVER INTERNAL] Warning: Received SESSION_ESTABLISHED "
                     "in unexpected state: "
                  << static_cast<int>(session_state_) << std::endl;
      }
      session_state_ = SessionState::ACTIVE;
      handshake_retry_count_ = 0;      // Reset retries on success
      needs_status_timer_start = true; // Signal to start status timer
      std::cout << "[ROVER INTERNAL] Session ACTIVE." << std::endl;

      // Handshake complete, cancel the handshake retry timer.
      boost::system::error_code ec;
      handshake_timer_.cancel(ec);

    } else {
      std::cout << "[ROVER INTERNAL] Ignoring SESSION_ESTABLISHED, session "
                   "already ACTIVE."
                << std::endl;
    }

  } // State Mutex Lock Released

  // Start the periodic status timer *after* releasing the lock if needed.
  if (needs_status_timer_start) {
    std::cout << "[ROVER INTERNAL] Starting periodic status timer."
              << std::endl;
    handle_status_timer(); // Start the timer loop
  }
}

// Handler for the periodic status update timer.
void Rover::handle_status_timer() {
  // Send the current status.
  send_status();

  // Reschedule the timer only if the session remains active.
  bool is_active;
  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);
    is_active = (session_state_ == SessionState::ACTIVE);
  } // State Mutex Lock Released

  if (is_active) {
    boost::system::error_code ec;
    status_timer_.expires_after(STATUS_INTERVAL); // Use constant from header
    status_timer_.async_wait([this](const boost::system::error_code &ec) {
      // Check error code (e.g., operation_aborted) and if still active
      if (!ec && get_session_state() == SessionState::ACTIVE) {
        handle_status_timer();
      } else if (ec) {
        // Timer cancelled or other error
        // std::cout << "[ROVER INTERNAL] Status timer wait interrupted: " <<
        // ec.message() << std::endl; // Debug log
      } else {
        // Session became inactive
        std::cout << "[ROVER INTERNAL] Status timer stopping (session no "
                     "longer active)."
                  << std::endl;
      }
    });
  } else {
    std::cout
        << "[ROVER INTERNAL] Status timer not rescheduled (session inactive)."
        << std::endl;
  }
}

// Sends a message bypassing LumenProtocol (sends raw JSON via UDP).
void Rover::send_raw_message(const Message &message,
                             const udp::endpoint &recipient) {
  if (!message_manager_) {
    std::cerr
        << "[ROVER] Error: Cannot send raw message, MessageManager is null."
        << std::endl;
    return;
  }
  if (recipient.address().is_unspecified() || recipient.port() == 0) {
    std::cerr
        << "[ROVER] Error: Invalid recipient endpoint for send_raw_message."
        << std::endl;
    return;
  }
  // Delegate raw sending to MessageManager
  message_manager_->send_raw_message(message, recipient);
}

const udp::endpoint &Rover::get_base_endpoint() const {
  if (!client_) {
    // This should ideally not happen if constructor succeeded.
    throw std::runtime_error("[ROVER] Error: UdpClient is not initialized.");
  }
  return client_->get_base_endpoint();
}

// Sends a standard application message via the full protocol stack to the base
// station.
void Rover::send_message(const Message &message) {
  bool can_send = false;
  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);
    can_send = (session_state_ == SessionState::ACTIVE);
  } // State Mutex Lock Released

  if (can_send) {
    if (message_manager_) {
      // Delegate sending to MessageManager; it sends to the default base
      // endpoint.
      message_manager_->send_message(message);
      // std::cout << "[ROVER] Sent message type: " << message.get_type() << "
      // to base." << std::endl; // Reduced verbosity
    } else {
      std::cerr << "[ROVER] Error: Cannot send message type "
                << message.get_type() << ", MessageManager is null."
                << std::endl;
    }
  } else {
    std::cout << "[ROVER] Cannot send message type " << message.get_type()
              << ", session not active." << std::endl;
  }
}