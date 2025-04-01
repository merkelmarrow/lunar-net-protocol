#include "base_station.hpp"

#include "basic_message.hpp" // Include BasicMessage if needed
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

  // set up message callback to route messages
  message_manager_->set_message_callback(
      [this](std::unique_ptr<Message> message, const udp::endpoint &sender) {
        route_message(std::move(message), sender); // Use the router function
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
    rover_endpoint_ = udp::endpoint(); // Clear endpoint
  }

  std::cout << "[BASE STATION] Stopped" << std::endl;
}

// New method to set the application-level message handler
void BaseStation::set_application_message_handler(
    ApplicationMessageHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  application_message_handler_ = std::move(handler);
  std::cout << "[BASE STATION] Application message handler set." << std::endl;
}

// Keep set_status_callback if desired for specific handling
void BaseStation::set_status_callback(StatusCallback callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  status_callback_ = std::move(callback);
  std::cout << "[BASE STATION] Status/Telemetry callback set." << std::endl;
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

// Renamed/Refactored: Routes message to internal handler or application handler
void BaseStation::route_message(std::unique_ptr<Message> message,
                                const udp::endpoint &sender) {
  if (!message)
    return;

  std::string msg_type = message->get_type();
  std::string sender_id = message->get_sender();

  std::cout << "[BASE INTERNAL] Routing message type: " << msg_type
            << " from: " << sender_id << " at: " << sender << std::endl;

  // --- Internal Handling Section ---

  // 1. Session Management Commands MUST be handled internally first
  if (msg_type == CommandMessage::message_type()) {
    auto cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    if (cmd_msg) {
      std::string command = cmd_msg->get_command();
      if (command == "SESSION_INIT" || command == "SESSION_CONFIRM") {
        handle_internal_command(cmd_msg, sender);
        return; // Don't pass session commands to application handler
      }
    }
  }

  // 2. Verify Session State for non-session-init messages
  // Allow SESSION_INIT even if inactive. For others, require ACTIVE session
  // from the correct rover.
  bool allow_message = false;
  if (msg_type == CommandMessage::message_type() &&
      dynamic_cast<CommandMessage *>(message.get())->get_command() ==
          "SESSION_INIT") {
    allow_message = true; // Allow SESSION_INIT anytime
  } else {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (session_state_ == SessionState::ACTIVE &&
        connected_rover_id_ == sender_id && rover_endpoint_ == sender) {
      allow_message = true;
    } else if (session_state_ == SessionState::HANDSHAKE_ACCEPT &&
               msg_type == CommandMessage::message_type() &&
               dynamic_cast<CommandMessage *>(message.get())->get_command() ==
                   "SESSION_CONFIRM" &&
               connected_rover_id_ == sender_id && rover_endpoint_ == sender) {
      allow_message = true; // Allow SESSION_CONFIRM during handshake
    } else {
      std::cout << "[BASE INTERNAL] Ignoring message type " << msg_type
                << " from " << sender_id << " at " << sender
                << ". Session State: " << static_cast<int>(session_state_)
                << ", Expected Rover: " << connected_rover_id_
                << ", Expected Endpoint: " << rover_endpoint_ << std::endl;
    }
  }

  if (!allow_message) {
    return; // Ignore message if session/sender doesn't match (unless it's
            // SESSION_INIT)
  }

  // 3. Handle Status/Telemetry internally if status_callback_ is set
  if (msg_type == StatusMessage::message_type()) {
    handle_internal_status(dynamic_cast<StatusMessage *>(message.get()),
                           sender);
    // Decide if you ALSO want to pass it to the generic application handler
    // below return; // Uncomment if status_callback is exclusive
  } else if (msg_type == TelemetryMessage::message_type()) {
    handle_internal_telemetry(dynamic_cast<TelemetryMessage *>(message.get()),
                              sender);
    // Decide if you ALSO want to pass it to the generic application handler
    // below
    // return; // Uncomment if status_callback is exclusive
  }

  // --- Application Handler Section ---

  // Pass all other messages (or Status/Telemetry if not exclusively handled
  // above) to the application handler if it's set.
  ApplicationMessageHandler handler_copy;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler_copy = application_message_handler_;
  }

  if (handler_copy) {
    std::cout << "[BASE INTERNAL] Passing message type " << msg_type
              << " to application handler." << std::endl;
    // Use post to avoid blocking the network thread
    boost::asio::post(io_context_,
                      [handler = std::move(handler_copy),
                       msg = std::move(message),
                       ep = sender]() mutable { handler(std::move(msg), ep); });
  } else {
    // Only log if it wasn't a status/telemetry message (which might be handled
    // by status_callback_)
    if (msg_type != StatusMessage::message_type() &&
        msg_type != TelemetryMessage::message_type()) {
      std::cout
          << "[BASE INTERNAL] No application handler set for message type: "
          << msg_type << std::endl;
    }
  }
}

// Internal handler for session commands
void BaseStation::handle_internal_command(CommandMessage *cmd_msg,
                                          const udp::endpoint &sender) {
  if (!cmd_msg)
    return;
  std::string command = cmd_msg->get_command();
  std::string params = cmd_msg->get_params(); // Often the rover_id
  std::string sender_id = cmd_msg->get_sender();

  std::cout << "[BASE INTERNAL] Handling command: " << command << " from "
            << sender_id << std::endl;

  if (command == "SESSION_INIT") {
    handle_session_init(sender_id, sender);
  } else if (command == "SESSION_CONFIRM") {
    handle_session_confirm(sender_id, sender);
  }
  // Add other internal commands if needed
}

// Internal handler for status messages (calls status_callback_)
void BaseStation::handle_internal_status(StatusMessage *status_msg,
                                         const udp::endpoint &sender) {
  if (!status_msg)
    return;
  std::string sender_id = status_msg->get_sender();

  std::cout << "[BASE INTERNAL] Processing Status from " << sender_id
            << ": Level=" << static_cast<int>(status_msg->get_level())
            << ", Desc=" << status_msg->get_description() << std::endl;

  StatusCallback callback_copy;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_copy = status_callback_;
  }
  if (callback_copy) {
    std::map<std::string, double> status_data = {
        {"status_level", static_cast<double>(status_msg->get_level())}
        // Add description if needed, though map expects doubles
        // {"description", status_msg->get_description()} // Needs map<string,
        // variant> or similar
    };
    // Use post to avoid blocking network thread
    boost::asio::post(io_context_,
                      [cb = std::move(callback_copy), id = sender_id,
                       data = std::move(status_data)]() { cb(id, data); });
  }
}

// Internal handler for telemetry messages (calls status_callback_)
void BaseStation::handle_internal_telemetry(TelemetryMessage *telemetry_msg,
                                            const udp::endpoint &sender) {
  if (!telemetry_msg)
    return;
  std::string sender_id = telemetry_msg->get_sender();
  const auto &readings = telemetry_msg->get_readings();

  std::cout << "[BASE INTERNAL] Processing Telemetry from " << sender_id << " ("
            << readings.size() << " readings)" << std::endl;

  StatusCallback callback_copy;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_copy = status_callback_;
  }
  if (callback_copy) {
    // Use post to avoid blocking network thread
    boost::asio::post(io_context_,
                      [cb = std::move(callback_copy), id = sender_id,
                       data = readings]() { cb(id, data); });
  }
}

// --- Session Management Logic (Remains Internal) ---

void BaseStation::handle_session_init(const std::string &rover_id,
                                      const udp::endpoint &sender) {
  std::cout << "[BASE INTERNAL] Received SESSION_INIT from rover: " << rover_id
            << " at " << sender << std::endl; // [cite: 1]

  bool send_accept = false; // Flag to indicate if we should send SESSION_ACCEPT

  {
    std::lock_guard<std::mutex> lock(state_mutex_); // [cite: 1]

    // If already in a session with a *different* rover, reject. Allow reconnect
    // from same rover.
    if (session_state_ != SessionState::INACTIVE &&
        connected_rover_id_ != rover_id) { // [cite: 1]
      std::cout << "[BASE INTERNAL] Rejecting new session from " << rover_id
                << ", already connected to " << connected_rover_id_
                << std::endl; // [cite: 1]
      // Optionally send a REJECT command (requires defining it)
      return; // [cite: 1]
    }

    // If reconnecting from the same rover, reset state
    if (connected_rover_id_ == rover_id &&
        rover_endpoint_ != sender) { // [cite: 1]
      std::cout << "[BASE INTERNAL] Rover " << rover_id
                << " reconnected from new endpoint " << sender
                << std::endl;                     // [cite: 1]
    } else if (connected_rover_id_ == rover_id) { // [cite: 1]
      std::cout << "[BASE INTERNAL] Rover " << rover_id
                << " re-initiating session." << std::endl; // [cite: 1]
    }

    // Update state and store rover details
    session_state_ = SessionState::HANDSHAKE_INIT; // [cite: 1]
    connected_rover_id_ = rover_id;                // [cite: 1]
    rover_endpoint_ = sender; // Store the sender's endpoint [cite: 1]
    std::cout << "[BASE INTERNAL] Rover endpoint set to: " << rover_endpoint_
              << std::endl; // [cite: 1]

    // ** FIX: Update state to HANDSHAKE_ACCEPT *before* sending the command **
    session_state_ = SessionState::HANDSHAKE_ACCEPT; // [cite: 1]
    send_accept = true;                              // Set flag to send command

  } // Mutex lock released here

  // Send SESSION_ACCEPT back to the rover's endpoint if flagged
  if (send_accept) {
    // The state check inside send_command will now pass
    send_command(
        "SESSION_ACCEPT",
        station_id_); // Send base station ID back maybe? Or just ack. [cite: 1]

    // Log after attempting to send
    std::cout << "[BASE INTERNAL] Sent SESSION_ACCEPT to rover: " << rover_id
              << std::endl; // [cite: 1]
  }
}

void BaseStation::handle_session_confirm(const std::string &rover_id,
                                         const udp::endpoint &sender) {
  std::cout << "[BASE INTERNAL] Received SESSION_CONFIRM from rover: "
            << rover_id << std::endl;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // Verify this is the rover we're handshaking with and it's the right state
    if (session_state_ != SessionState::HANDSHAKE_ACCEPT ||
        connected_rover_id_ != rover_id || rover_endpoint_ != sender) {
      std::cout
          << "[BASE INTERNAL] Ignoring SESSION_CONFIRM from unexpected rover ("
          << rover_id << " at " << sender << ")"
          << " or in wrong state (" << static_cast<int>(session_state_) << ")."
          << std::endl;
      return;
    }

    // Update state to ACTIVE
    session_state_ = SessionState::ACTIVE;
  }

  std::cout << "[BASE INTERNAL] Session ACTIVE with rover: " << rover_id
            << std::endl;

  // Send a final confirmation back to the rover
  send_command("SESSION_ESTABLISHED", station_id_);
  std::cout << "[BASE INTERNAL] Sent SESSION_ESTABLISHED to rover: " << rover_id
            << std::endl;
}

// Send command TO THE CONNECTED ROVER
void BaseStation::send_command(const std::string &command,
                               const std::string &params) {
  CommandMessage cmd_msg(command, params, station_id_);
  udp::endpoint target_endpoint;
  bool is_active = false;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Allow sending SESSION_ACCEPT even if not fully active yet
    if (session_state_ == SessionState::HANDSHAKE_ACCEPT ||
        session_state_ == SessionState::ACTIVE) {
      if (rover_endpoint_.address().is_unspecified()) {
        std::cerr << "[BASE STATION] Cannot send command '" << command
                  << "', rover endpoint unknown." << std::endl;
        return;
      }
      target_endpoint = rover_endpoint_;
      is_active = true; // Technically active enough to send response/command
    }
  } // Mutex released

  if (is_active) {
    std::cout << "[BASE STATION] Sending command '" << command << "' to "
              << target_endpoint << std::endl;
    message_manager_->send_message(cmd_msg, target_endpoint);
  } else {
    std::cerr << "[BASE STATION] Cannot send command '" << command
              << "', session not active or initializing." << std::endl;
  }
}

void BaseStation::send_raw_message(const Message &message,
                                   const udp::endpoint &endpoint) {
  message_manager_->send_raw_message(message, endpoint);
}

void BaseStation::send_message(const Message &message,
                               const udp::endpoint &recipient) {
  if (!message_manager_) {
    std::cerr << "[BASE STATION] Error: Message Manager not initialized. "
                 "Cannot send message."
              << std::endl;
    return;
  }
  if (recipient.address().is_unspecified() || recipient.port() == 0) {
    std::cerr << "[BASE STATION] Error: Invalid recipient endpoint provided "
                 "for sending message type "
              << message.get_type() << std::endl;
    return;
  }

  // Use the internal message manager to send the message to the specified
  // recipient
  message_manager_->send_message(message, recipient);

  std::cout << "[BASE STATION] Sent message type " << message.get_type()
            << " to " << recipient << std::endl;
}