// src/base/base_station.cpp

#include "base_station.hpp"

#include "command_message.hpp"
#include "message_manager.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <string>

BaseStation::BaseStation(boost::asio::io_context &io_context, int port,
                         const std::string &station_id)
    : io_context_(io_context),
      session_state_(SessionState::INACTIVE), // start inactive
      station_id_(station_id) {

  // instantiate underlying layers in order: transport -> protocol -> message
  // manager
  server_ = std::make_unique<UdpServer>(io_context, port);
  protocol_ =
      std::make_unique<LumenProtocol>(io_context, *server_); // pass server ref
  message_manager_ = std::make_unique<MessageManager>(
      io_context, *protocol_, station_id_,
      server_.get()); // pass protocol, id, server ref

  std::cout << "[BASE STATION] Initialized on port " << port
            << " with ID: " << station_id << std::endl;
}

BaseStation::~BaseStation() {
  stop(); // ensure resources are released
}

void BaseStation::start() {
  if (message_manager_)
    message_manager_->start();
  if (protocol_) {
    protocol_->start();
    protocol_->set_session_active(false); // ensure inactive on start
  }
  if (server_)
    server_->start(); // start server last (innermost layer)

  // set up callback chain: messagemanager passes deserialized messages up
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
  if (protocol_) {
    protocol_->set_session_active(false);
  }
  // stop layers in reverse order
  if (server_)
    server_->stop();
  if (protocol_)
    protocol_->stop();
  if (message_manager_)
    message_manager_->stop();

  // reset session state
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_state_ = SessionState::INACTIVE;
    connected_rover_id_.clear();
    rover_endpoint_ = udp::endpoint(); // clear stored endpoint
  }

  std::cout << "[BASE STATION] Stopped" << std::endl;
}

// set a specific handler for status/telemetry messages
void BaseStation::set_status_callback(StatusCallback callback) {
  std::lock_guard<std::mutex> lock(
      callback_mutex_); // protects status_callback_
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

// central router for messages received from messagemanager
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

  // --- internal handling ---
  // certain messages must be handled internally, primarily session management

  // 1. handle session management commands
  if (msg_type == CommandMessage::message_type()) {
    auto *cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    if (cmd_msg) {
      const std::string &command = cmd_msg->get_command();
      // session_init and session_confirm are critical internal commands
      if (command == "SESSION_INIT" || command == "SESSION_CONFIRM") {
        handle_internal_command(cmd_msg, sender);
        // session commands fully handled internally, do not pass further
        return;
      }
    }
  }

  // 2. session/sender verification for non-session-init messages
  // ensure message is from expected rover and session is active (or
  // initializing for confirm)
  bool allow_message = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (session_state_ == SessionState::ACTIVE) {
      // in active state, message must be from connected rover's id and endpoint
      allow_message =
          (connected_rover_id_ == sender_id && rover_endpoint_ == sender);
    } else if (session_state_ == SessionState::HANDSHAKE_ACCEPT) {
      // during handshake accept state, only allow session_confirm from expected
      // rover
      if (msg_type == CommandMessage::message_type()) {
        auto *cmd_msg = dynamic_cast<CommandMessage *>(message.get());
        allow_message =
            (cmd_msg && cmd_msg->get_command() == "SESSION_CONFIRM" &&
             connected_rover_id_ == sender_id && rover_endpoint_ == sender);
      }
    }
    // note: session_init bypasses this check as handled earlier
  }

  if (!allow_message) {
    std::cout << "[BASE INTERNAL] Ignoring message type '" << msg_type
              << "' from " << sender_id << " at " << sender
              << ". Reason: Session inactive or mismatch with expected "
                 "rover/endpoint."
              << std::endl;
    return; // ignore message
  }

  // 3. optional internal handling for status/telemetry via dedicated callback
  StatusCallback status_cb_copy;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    status_cb_copy = status_callback_;
  }

  if (status_cb_copy) {
    if (msg_type == StatusMessage::message_type()) {
      handle_internal_status(dynamic_cast<StatusMessage *>(message.get()),
                             status_cb_copy);
      // decide if status messages handled here should also go to general
      // handler return; // uncomment to make status_callback_ exclusive
    } else if (msg_type == TelemetryMessage::message_type()) {
      handle_internal_telemetry(dynamic_cast<TelemetryMessage *>(message.get()),
                                status_cb_copy);
      // decide if telemetry messages handled here should also go to general
      // handler return; // uncomment to make status_callback_ exclusive
    }
  }

  // --- application handling ---
  // pass remaining messages to general application handler if registered
  ApplicationMessageHandler handler_copy;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler_copy = application_message_handler_;
  }

  if (handler_copy) {
    // post to io_context to ensure handler runs on io_context thread without
    // blocking network thread
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
    // log if message wasn't handled by any callback
    bool was_status_telemetry = (msg_type == StatusMessage::message_type() ||
                                 msg_type == TelemetryMessage::message_type());
    if (!status_cb_copy || !was_status_telemetry) {
      std::cout << "[BASE INTERNAL] No application handler registered for "
                   "message type '"
                << msg_type << "' from " << sender_id << "." << std::endl;
    }
  }
}

// set the handler for general application messages (non-internal)
void BaseStation::set_application_message_handler(
    ApplicationMessageHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  application_message_handler_ = std::move(handler);
  std::cout << "[BASE STATION] Application message handler registered."
            << std::endl;
}

// handles internal session-related commands
void BaseStation::handle_internal_command(CommandMessage *cmd_msg,
                                          const udp::endpoint &sender) {
  if (!cmd_msg)
    return;
  const std::string &command = cmd_msg->get_command();
  const std::string &params =
      cmd_msg->get_params(); // rover id for session commands
  const std::string &sender_id =
      cmd_msg->get_sender(); // get sender id from message

  std::cout << "[BASE INTERNAL] Handling command: '" << command
            << "' from sender ID: '" << sender_id << "' at " << sender
            << std::endl;

  if (command == "SESSION_INIT") {
    handle_session_init(sender_id, sender); // pass sender_id from message
  } else if (command == "SESSION_CONFIRM") {
    handle_session_confirm(sender_id, sender); // pass sender_id from message
  }
}

// internal processor for statusmessages, invokes specific status_callback_
void BaseStation::handle_internal_status(StatusMessage *status_msg,
                                         const StatusCallback &callback) {
  if (!status_msg || !callback)
    return;

  std::string sender_id = status_msg->get_sender();

  // convert status info to map format expected by callback
  std::map<std::string, double> status_data;
  status_data["status_level"] = static_cast<double>(status_msg->get_level());
  // note: description cannot be included in this map<string, double> format

  // post callback execution to io_context thread pool
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

// internal processor for telemetrymessages, invokes specific status_callback_
void BaseStation::handle_internal_telemetry(TelemetryMessage *telemetry_msg,
                                            const StatusCallback &callback) {
  if (!telemetry_msg || !callback)
    return;

  std::string sender_id = telemetry_msg->get_sender();
  const auto &readings =
      telemetry_msg->get_readings(); // get telemetry data map

  // post callback execution to io_context thread pool
  boost::asio::post(io_context_, [cb = callback, id = sender_id,
                                  data = readings]() { // pass readings map
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

// --- session management implementation ---

void BaseStation::handle_session_init(const std::string &rover_id,
                                      const udp::endpoint &sender) {
  std::cout << "[BASE INTERNAL] Received SESSION_INIT from rover ID: '"
            << rover_id << "' at " << sender << std::endl;

  // check current state first
  SessionState current_local_state;
  std::string current_rover_id;
  udp::endpoint current_rover_endpoint;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_local_state = session_state_;
    current_rover_id = connected_rover_id_;
    current_rover_endpoint = rover_endpoint_;
  }

  // if already active with same rover and endpoint, resend established or
  // ignore
  if (current_local_state == SessionState::ACTIVE &&
      current_rover_id == rover_id && current_rover_endpoint == sender) {
    std::cout
        << "[BASE INTERNAL] Ignoring SESSION_INIT from '" << rover_id
        << "' as session is already ACTIVE. Re-sending SESSION_ESTABLISHED."
        << std::endl;
    send_command("SESSION_ESTABLISHED", station_id_); // re-confirm session
    return;
  }

  bool send_accept = false;
  bool reset_session = false; // flag if resetting due to collision/re-init
  {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (session_state_ != SessionState::INACTIVE &&
        connected_rover_id_ != rover_id) {
      std::cout << "[BASE INTERNAL] Rejecting SESSION_INIT from '" << rover_id
                << "'. Already in session with '" << connected_rover_id_ << "'."
                << std::endl;
      reset_session = (session_state_ == SessionState::ACTIVE);
    } else if (connected_rover_id_ == rover_id || connected_rover_id_.empty()) {
      // allow re-initiation if inactive, during handshake, or from changed
      // endpoint
      if (session_state_ == SessionState::ACTIVE &&
          connected_rover_id_ == rover_id) {
        // re-initiation while active: treat as reset
        reset_session = true;
        std::cout << "[BASE INTERNAL] Rover '" << rover_id
                  << "' re-initiating session while ACTIVE. Resetting state."
                  << std::endl;
      } else if (connected_rover_id_ == rover_id && rover_endpoint_ != sender) {
        std::cout << "[BASE INTERNAL] Rover '" << rover_id
                  << "' re-initiating session from new endpoint: " << sender
                  << "." << std::endl;
        if (session_state_ == SessionState::ACTIVE)
          reset_session = true; // reset if was active
      } else if (session_state_ == SessionState::INACTIVE) {
        std::cout << "[BASE INTERNAL] New session initiation from '" << rover_id
                  << "'." << std::endl;
      } else {
        std::cout
            << "[BASE INTERNAL] Rover '" << rover_id
            << "' re-initiating session from same endpoint while in state "
            << static_cast<int>(session_state_) << "." << std::endl;
        if (session_state_ == SessionState::ACTIVE)
          reset_session = true; // reset if was active
      }

      // update state and store initiating rover's details
      session_state_ = SessionState::HANDSHAKE_ACCEPT;
      connected_rover_id_ = rover_id;
      rover_endpoint_ = sender;
      send_accept = true;

      std::cout << "[BASE INTERNAL] State set to HANDSHAKE_ACCEPT. Stored "
                   "rover endpoint: "
                << rover_endpoint_ << std::endl;
    } else {
      reset_session = (session_state_ ==
                       SessionState::ACTIVE); // ensure reset if we were active
    }
  }

  if (reset_session && protocol_) {
    protocol_->set_session_active(false);
  }

  // send session_accept back if flagged
  if (send_accept) {
    send_command("SESSION_ACCEPT", station_id_);
    std::cout << "[BASE INTERNAL] Sent SESSION_ACCEPT to rover '" << rover_id
              << "' at " << sender << "." << std::endl;
  } else if (reset_session) {
    std::cout << "[BASE INTERNAL] Session initiation from '" << rover_id
              << "' rejected or caused reset." << std::endl;
  }
}

void BaseStation::handle_session_confirm(const std::string &rover_id,
                                         const udp::endpoint &sender) {
  std::cout << "[BASE INTERNAL] Received SESSION_CONFIRM from rover ID: '"
            << rover_id << "' at " << sender << std::endl;

  bool send_established = false; // flag to send session_established

  { // state mutex lock scope
    std::lock_guard<std::mutex> lock(state_mutex_);

    // verify confirm is from expected rover during correct handshake phase
    if (session_state_ != SessionState::HANDSHAKE_ACCEPT ||
        connected_rover_id_ != rover_id || rover_endpoint_ != sender) {
      std::cout << "[BASE INTERNAL] Ignoring SESSION_CONFIRM from '" << rover_id
                << "' at " << sender << ". Reason: State ("
                << static_cast<int>(session_state_)
                << ") or rover mismatch (Expected: '" << connected_rover_id_
                << "' at " << rover_endpoint_ << ")." << std::endl;
      return; // ignore invalid confirmation
    }

    // handshake successful, transition to active state
    session_state_ = SessionState::ACTIVE;
    send_established = true; // mark to send session_established

    std::cout << "[BASE INTERNAL] Session ACTIVE with rover: '"
              << connected_rover_id_ << "' at " << rover_endpoint_ << std::endl;

    if (protocol_) {
      protocol_->set_session_active(true);
    }

  } // state mutex lock released

  // send final session_established confirmation if flagged
  if (send_established) {
    send_command("SESSION_ESTABLISHED", station_id_);
    std::cout << "[BASE INTERNAL] Sent SESSION_ESTABLISHED to rover '"
              << rover_id << "'." << std::endl;
  }
}

// sends a command message to the connected rover
void BaseStation::send_command(const std::string &command,
                               const std::string &params) {
  udp::endpoint target_endpoint;
  bool can_send = false;
  std::string current_rover;

  { // state mutex lock scope
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_rover = connected_rover_id_;

    bool is_session_command =
        (command == "SESSION_ACCEPT" || command == "SESSION_ESTABLISHED");

    // check if session state allows sending this command
    if (session_state_ == SessionState::ACTIVE ||
        (session_state_ == SessionState::HANDSHAKE_ACCEPT &&
         command == "SESSION_ACCEPT") ||
        (session_state_ == SessionState::ACTIVE &&
         command == "SESSION_ESTABLISHED")) {
      if (rover_endpoint_.address().is_unspecified()) {
        std::cerr << "[BASE STATION] Error: Cannot send command '" << command
                  << "'. Rover endpoint is unknown." << std::endl;
      } else {
        // only allow non-session commands if fully active
        if (!is_session_command && session_state_ != SessionState::ACTIVE) {
          std::cerr << "[BASE STATION] Cannot send command '" << command
                    << "'. Session must be ACTIVE." << std::endl;
        } else {
          target_endpoint = rover_endpoint_;
          can_send = true;
        }
      }
    }
  } // state mutex lock released

  if (can_send) {
    CommandMessage cmd_msg(command, params,
                           station_id_); // create message object
    if (message_manager_) {
      std::cout << "[BASE STATION] Sending command '" << command
                << "' to rover '" << current_rover << "' at " << target_endpoint
                << std::endl;
      message_manager_->send_message(cmd_msg, target_endpoint);
    } else {
      std::cerr << "[BASE STATION] Error: Cannot send command '" << command
                << "', MessageManager is null." << std::endl;
    }
  } else {
    // log why sending failed
    SessionState current_state_local = get_session_state(); // get state safely
    std::cerr << "[BASE STATION] Cannot send command '" << command
              << "'. Session not in appropriate state (Current: "
              << static_cast<int>(current_state_local)
              << ", Rover: " << get_connected_rover_id() << ")." << std::endl;
  }
}

// sends a message bypassing lumenprotocol (sends raw json via udp)
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
  // delegate raw sending to messagemanager
  message_manager_->send_raw_message(message, recipient);
}

void BaseStation::set_low_power_mode(bool enable) {
  // command: set_low_power, params: "1" or "0"
  std::string params = enable ? "1" : "0";
  send_command("SET_LOW_POWER", params);
}

void BaseStation::set_rover_target(double latitude, double longitude) {
  // command: set_target_coord, params: "latitude,longitude"
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(6) << latitude << "," << longitude;
  std::string params = oss.str();
  send_command("SET_TARGET_COORD", params);
}

// sends a standard application message via the full protocol stack to a
// specific recipient
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

  // delegate sending to messagemanager, ensuring recipient is specified
  message_manager_->send_message(message, recipient);
}