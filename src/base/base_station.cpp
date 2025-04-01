// src/base/base_station.cpp

#include "base_station.hpp"

#include "command_message.hpp"
#include "message_manager.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"

#include <iostream>
#include <map>    // For map in status/telemetry handling
#include <memory> // For std::unique_ptr, std::move
#include <string> // For std::string comparison etc.

BaseStation::BaseStation(boost::asio::io_context &io_context, int port,
                         const std::string &station_id)
    : io_context_(io_context),
      session_state_(SessionState::INACTIVE), // Start inactive
      station_id_(station_id) {

  // Instantiate the underlying layers in order: Transport -> Protocol ->
  // Message Manager
  server_ = std::make_unique<UdpServer>(io_context, port);
  protocol_ =
      std::make_unique<LumenProtocol>(io_context, *server_); // Pass server ref
  message_manager_ = std::make_unique<MessageManager>(
      io_context, *protocol_, station_id_,
      server_.get()); // Pass protocol, id, server ref

  std::cout << "[BASE STATION] Initialized on port " << port
            << " with ID: " << station_id << std::endl;
}

BaseStation::~BaseStation() {
  stop(); // Ensure resources are released
}

void BaseStation::start() {
  if (message_manager_)
    message_manager_->start();
  if (protocol_)
    protocol_->start();
  if (server_)
    server_->start(); // Start server last (innermost layer)

  // Set up the callback chain: MessageManager receives deserialized messages
  // and passes them up to BaseStation's route_message method.
  if (message_manager_) {
    message_manager_->set_message_callback(
        [this](std::unique_ptr<Message> message, const udp::endpoint &sender) {
          route_message(std::move(message), sender);
        });
  } else {
    std::cerr << "[BASE STATION] Error: Cannot set message callback, "
                 "MessageManager is null."
              << std::endl;
  }

  std::cout << "[BASE STATION] Started and listening for connections"
            << std::endl;
}

void BaseStation::stop() {
  // Stop layers in reverse order of initialization/start
  if (server_)
    server_->stop();
  if (protocol_)
    protocol_->stop();
  if (message_manager_)
    message_manager_->stop();

  // Reset session state
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_state_ = SessionState::INACTIVE;
    connected_rover_id_.clear();
    rover_endpoint_ = udp::endpoint(); // Clear stored endpoint
  }

  std::cout << "[BASE STATION] Stopped" << std::endl;
}

// Set the handler for general application messages (non-internal)
void BaseStation::set_application_message_handler(
    ApplicationMessageHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  application_message_handler_ = std::move(handler);
  std::cout << "[BASE STATION] Application message handler registered."
            << std::endl;
}

// Set a specific handler for Status/Telemetry messages
void BaseStation::set_status_callback(StatusCallback callback) {
  std::lock_guard<std::mutex> lock(
      callback_mutex_); // Protects status_callback_
  status_callback_ = std::move(callback);
  std::cout << "[BASE STATION] Status/Telemetry callback registered."
            << std::endl;
}

BaseStation::SessionState BaseStation::get_session_state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return session_state_;
}

std::string BaseStation::get_connected_rover_id() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return connected_rover_id_;
}

udp::endpoint BaseStation::get_rover_endpoint() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return rover_endpoint_;
}

// Central router for messages received from MessageManager
void BaseStation::route_message(std::unique_ptr<Message> message,
                                const udp::endpoint &sender) {
  if (!message) {
    std::cerr << "[BASE STATION] Error: Received null message pointer in "
                 "route_message."
              << std::endl;
    return;
  }

  std::string msg_type = message->get_type();
  std::string sender_id = message->get_sender();

  // --- Internal Handling ---
  // Certain messages MUST be handled internally, primarily session management.

  // 1. Handle Session Management Commands
  if (msg_type == CommandMessage::message_type()) {
    auto *cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    if (cmd_msg) {
      const std::string &command = cmd_msg->get_command();
      // SESSION_INIT and SESSION_CONFIRM are critical internal commands.
      if (command == "SESSION_INIT" || command == "SESSION_CONFIRM") {
        handle_internal_command(cmd_msg, sender);
        // Session commands are fully handled internally, do not pass further.
        return;
      }
    }
  }

  // 2. Session/Sender Verification for non-session-init messages
  // Ensure the message is from the expected rover and the session is active (or
  // initializing for CONFIRM).
  bool allow_message = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (session_state_ == SessionState::ACTIVE) {
      // In ACTIVE state, message must be from the connected rover's ID and
      // endpoint.
      allow_message =
          (connected_rover_id_ == sender_id && rover_endpoint_ == sender);
    } else if (session_state_ == SessionState::HANDSHAKE_ACCEPT) {
      // During handshake accept state, only allow SESSION_CONFIRM from the
      // expected rover.
      if (msg_type == CommandMessage::message_type()) {
        auto *cmd_msg = dynamic_cast<CommandMessage *>(message.get());
        allow_message =
            (cmd_msg && cmd_msg->get_command() == "SESSION_CONFIRM" &&
             connected_rover_id_ == sender_id && rover_endpoint_ == sender);
      }
    }
    // Note: SESSION_INIT bypasses this check as it was handled earlier.
  }

  if (!allow_message) {
    std::cout << "[BASE INTERNAL] Ignoring message type '" << msg_type
              << "' from " << sender_id << " at " << sender
              << ". Reason: Session inactive or mismatch with expected "
                 "rover/endpoint."
              << std::endl;
    return; // Ignore message
  }

  // 3. Optional Internal Handling for Status/Telemetry via dedicated callback
  StatusCallback status_cb_copy; // Copy callback for thread safety
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    status_cb_copy = status_callback_;
  }

  if (status_cb_copy) {
    if (msg_type == StatusMessage::message_type()) {
      handle_internal_status(dynamic_cast<StatusMessage *>(message.get()),
                             status_cb_copy);
      // Decide whether status messages handled by status_callback_ should ALSO
      // go to the general handler.

      // return; // Uncomment to make status_callback_ exclusive
    } else if (msg_type == TelemetryMessage::message_type()) {
      handle_internal_telemetry(dynamic_cast<TelemetryMessage *>(message.get()),
                                status_cb_copy);
      // Decide whether telemetry messages handled by status_callback_ should
      // ALSO go to the general handler.

      // return; // Uncomment to make status_callback_ exclusive
    }
  }

  // --- Application Handling ---
  // Pass any remaining messages (or those not exclusively handled above)
  // to the general application message handler, if registered.
  ApplicationMessageHandler handler_copy;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler_copy = application_message_handler_;
  }

  if (handler_copy) {
    // boost::asio::post to ensure the handler runs on the io_context thread
    // without blocking the current network processing thread.
    boost::asio::post(io_context_, [handler = std::move(handler_copy),
                                    msg = std::move(message),
                                    ep = sender]() mutable {
      try {
        handler(std::move(msg), ep);
      } catch (const std::exception &e) {
        std::cerr
            << "[ERROR] BaseStation: Exception in application message handler: "
            << e.what() << std::endl;
      }
    });
  } else {
    // Log if a message wasn't handled by any callback (and wasn't expected to
    // be handled internally). Avoid logging for Status/Telemetry if
    // status_callback_ exists but wasn't exclusive.
    bool was_status_telemetry = (msg_type == StatusMessage::message_type() ||
                                 msg_type == TelemetryMessage::message_type());
    if (!status_cb_copy || !was_status_telemetry) {
      std::cout << "[BASE INTERNAL] No application handler registered for "
                   "message type '"
                << msg_type << "' from " << sender_id << "." << std::endl;
    }
  }
}

// Handles internal session-related commands
void BaseStation::handle_internal_command(CommandMessage *cmd_msg,
                                          const udp::endpoint &sender) {
  if (!cmd_msg)
    return;
  const std::string &command = cmd_msg->get_command();
  const std::string &params =
      cmd_msg->get_params(); // Rover ID for session commands
  const std::string &sender_id =
      cmd_msg->get_sender(); // Get sender ID from message object

  std::cout << "[BASE INTERNAL] Handling command: '" << command
            << "' from sender ID: '" << sender_id << "' at " << sender
            << std::endl;

  if (command == "SESSION_INIT") {
    // Pass the sender_id from the message, not the params.
    handle_session_init(sender_id, sender);
  } else if (command == "SESSION_CONFIRM") {
    // Pass the sender_id from the message.
    handle_session_confirm(sender_id, sender);
  }
}

// Internal processor for StatusMessages, invokes the specific status_callback_
void BaseStation::handle_internal_status(StatusMessage *status_msg,
                                         const StatusCallback &callback) {
  if (!status_msg || !callback)
    return;

  std::string sender_id = status_msg->get_sender();

  // Convert status info to the map format expected by the callback
  std::map<std::string, double> status_data;
  status_data["status_level"] = static_cast<double>(status_msg->get_level());
  // Note: Description is string, map is double. Cannot directly include
  // description in this callback format.

  // Post the callback execution to the io_context thread pool
  boost::asio::post(io_context_, [cb = callback, id = sender_id,
                                  data = std::move(status_data)]() {
    try {
      cb(id, data);
    } catch (const std::exception &e) {
      std::cerr << "[ERROR] BaseStation: Exception in status callback: "
                << e.what() << std::endl;
    }
  });
}

// Internal processor for TelemetryMessages, invokes the specific
// status_callback_
void BaseStation::handle_internal_telemetry(TelemetryMessage *telemetry_msg,
                                            const StatusCallback &callback) {
  if (!telemetry_msg || !callback)
    return;

  std::string sender_id = telemetry_msg->get_sender();
  const auto &readings =
      telemetry_msg->get_readings(); // Get telemetry data map

  // Post the callback execution to the io_context thread pool
  boost::asio::post(io_context_, [cb = callback, id = sender_id,
                                  data = readings]() { // Pass readings map
                                                       // directly
    try {
      cb(id, data);
    } catch (const std::exception &e) {
      std::cerr
          << "[ERROR] BaseStation: Exception in telemetry (status) callback: "
          << e.what() << std::endl;
    }
  });
}

// --- Session Management Implementation ---

void BaseStation::handle_session_init(const std::string &rover_id,
                                      const udp::endpoint &sender) {
  std::cout << "[BASE INTERNAL] Received SESSION_INIT from rover ID: '"
            << rover_id << "' at " << sender << std::endl;

  bool send_accept = false; // Flag to send SESSION_ACCEPT outside the lock

  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);

    // Check for existing session collision
    if (session_state_ != SessionState::INACTIVE &&
        connected_rover_id_ != rover_id) {
      std::cout << "[BASE INTERNAL] Rejecting SESSION_INIT from '" << rover_id
                << "'. Already in session with '" << connected_rover_id_ << "'."
                << std::endl;
      // Consider sending a SESSION_REJECT command if defined.
      return; // Reject new session attempt
    }

    // Handle cases: New connection, reconnection from same rover
    if (connected_rover_id_ == rover_id) {
      if (rover_endpoint_ != sender) {
        std::cout << "[BASE INTERNAL] Rover '" << rover_id
                  << "' reconnected from new endpoint: " << sender << " (was "
                  << rover_endpoint_ << ")." << std::endl;
      } else {
        std::cout << "[BASE INTERNAL] Rover '" << rover_id
                  << "' re-initiating session from same endpoint." << std::endl;
      }
    } else {
      std::cout << "[BASE INTERNAL] New session initiation from '" << rover_id
                << "'." << std::endl;
    }

    // Update state and store initiating rover's details
    // Move to HANDSHAKE_ACCEPT state immediately before sending accept
    session_state_ = SessionState::HANDSHAKE_ACCEPT;
    connected_rover_id_ = rover_id;
    rover_endpoint_ = sender; // Store the endpoint we received INIT from
    send_accept = true;       // Mark that we should send SESSION_ACCEPT

    std::cout << "[BASE INTERNAL] State set to HANDSHAKE_ACCEPT. Stored rover "
                 "endpoint: "
              << rover_endpoint_ << std::endl;

  } // State Mutex Lock Released

  // Send SESSION_ACCEPT back to the initiating rover if flagged
  if (send_accept) {
    // Send base station ID in params? Or just a simple accept? Sending ID for
    // now.
    send_command("SESSION_ACCEPT", station_id_);
    std::cout << "[BASE INTERNAL] Sent SESSION_ACCEPT to rover '" << rover_id
              << "' at " << sender << "." << std::endl;
  }
}

void BaseStation::handle_session_confirm(const std::string &rover_id,
                                         const udp::endpoint &sender) {
  std::cout << "[BASE INTERNAL] Received SESSION_CONFIRM from rover ID: '"
            << rover_id << "' at " << sender << std::endl;

  bool send_established = false; // Flag to send SESSION_ESTABLISHED

  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);

    // Verify CONFIRM is from the expected rover during the correct handshake
    // phase.
    if (session_state_ != SessionState::HANDSHAKE_ACCEPT ||
        connected_rover_id_ != rover_id || rover_endpoint_ != sender) {
      std::cout << "[BASE INTERNAL] Ignoring SESSION_CONFIRM from '" << rover_id
                << "' at " << sender << ". Reason: State ("
                << static_cast<int>(session_state_)
                << ") or rover mismatch (Expected: '" << connected_rover_id_
                << "' at " << rover_endpoint_ << ")." << std::endl;
      return; // Ignore invalid confirmation
    }

    // Handshake successful, transition to ACTIVE state.
    session_state_ = SessionState::ACTIVE;
    send_established = true; // Mark that we should send SESSION_ESTABLISHED

    std::cout << "[BASE INTERNAL] Session ACTIVE with rover: '"
              << connected_rover_id_ << "' at " << rover_endpoint_ << std::endl;

  } // State Mutex Lock Released

  // Send final SESSION_ESTABLISHED confirmation if flagged
  if (send_established) {
    send_command("SESSION_ESTABLISHED", station_id_);
    std::cout << "[BASE INTERNAL] Sent SESSION_ESTABLISHED to rover '"
              << rover_id << "'." << std::endl;
  }
}

// Sends a command message to the connected rover
void BaseStation::send_command(const std::string &command,
                               const std::string &params) {
  udp::endpoint target_endpoint;
  bool can_send = false;

  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Commands can usually only be sent when session is ACTIVE.
    // Allow sending session responses (ACCEPT, ESTABLISHED) during handshake.
    if (session_state_ == SessionState::ACTIVE ||
        (session_state_ == SessionState::HANDSHAKE_ACCEPT &&
         command == "SESSION_ACCEPT") ||
        (session_state_ == SessionState::ACTIVE &&
         command == "SESSION_ESTABLISHED") // Technically ACTIVE when sending
                                           // ESTABLISHED
    ) {
      if (rover_endpoint_.address().is_unspecified()) {
        std::cerr << "[BASE STATION] Error: Cannot send command '" << command
                  << "'. Rover endpoint is unknown." << std::endl;
      } else {
        target_endpoint = rover_endpoint_;
        can_send = true;
      }
    }
  } // State Mutex Lock Released

  if (can_send) {
    CommandMessage cmd_msg(command, params,
                           station_id_); // Create message object
    if (message_manager_) {
      // Delegate sending to MessageManager, specifying the target endpoint
      message_manager_->send_message(cmd_msg, target_endpoint);
    } else {
      std::cerr << "[BASE STATION] Error: Cannot send command '" << command
                << "', MessageManager is null." << std::endl;
    }
  } else {
    std::cerr << "[BASE STATION] Cannot send command '" << command
              << "'. Session not in appropriate state ("
              << static_cast<int>(get_session_state()) << ")." << std::endl;
  }
}

// Sends a message bypassing LumenProtocol (sends raw JSON via UDP).
void BaseStation::send_raw_message(const Message &message,
                                   const udp::endpoint &recipient) {
  if (!message_manager_) {
    std::cerr << "[BASE STATION] Error: Cannot send raw message, "
                 "MessageManager is null."
              << std::endl;
    return;
  }
  if (recipient.address().is_unspecified() || recipient.port() == 0) {
    std::cerr << "[BASE STATION] Error: Invalid recipient endpoint for "
                 "send_raw_message."
              << std::endl;
    return;
  }
  // Delegate raw sending to MessageManager
  message_manager_->send_raw_message(message, recipient);
}

// Sends a standard application message via the full protocol stack to a
// specific recipient.
void BaseStation::send_message(const Message &message,
                               const udp::endpoint &recipient) {
  if (!message_manager_) {
    std::cerr
        << "[BASE STATION] Error: Cannot send message, MessageManager is null."
        << std::endl;
    return;
  }
  if (recipient.address().is_unspecified() || recipient.port() == 0) {
    std::cerr << "[BASE STATION] Error: Invalid recipient endpoint provided "
                 "for sending message type '"
              << message.get_type() << "'." << std::endl;
    return;
  }

  // Delegate sending to MessageManager, ensuring the recipient is specified.
  message_manager_->send_message(message, recipient);
}