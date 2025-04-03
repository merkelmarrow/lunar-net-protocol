// src/rover/rover.cpp

#include "rover.hpp"
#include "command_message.hpp"
#include "lumen_protocol.hpp"
#include "message_manager.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"
#include <boost/asio/post.hpp>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>

namespace { // Use anonymous namespace for internal linkage
std::string endpoint_to_string(const udp::endpoint &ep) {
  boost::system::error_code ec;
  std::string addr = ep.address().to_string(ec);
  if (ec) {
    addr = "invalid_address";
  }
  return addr + ":" + std::to_string(ep.port());
}
} // anonymous namespace

// --- Constructor ---
Rover::Rover(boost::asio::io_context &io_context, const std::string &base_host,
             int base_port, const std::string &rover_id)
    : io_context_(io_context), status_timer_(io_context),
      position_telemetry_timer_(io_context), handshake_timer_(io_context),
      probe_timer_(io_context), // **** ADDED: Initialize probe timer ****
      session_state_(SessionState::INACTIVE), rover_id_(rover_id),
      current_status_level_(StatusMessage::StatusLevel::OK),
      current_status_description_("System nominal"), handshake_retry_count_(0) {
  // Initialize communication layers
  client_ = std::make_unique<UdpClient>(
      io_context); // UdpClient handles broadcast setup internally now
  try {
    client_->register_base(base_host, base_port);
  } catch (const std::exception &e) {
    std::cerr << "[ROVER] CRITICAL ERROR during initialization: Failed to "
                 "register base station at "
              << base_host << ":" << base_port << ". Error: " << e.what()
              << std::endl;
    throw; // Propagate error
  }
  protocol_ = std::make_unique<LumenProtocol>(io_context, *client_);
  message_manager_ = std::make_unique<MessageManager>(
      io_context, *protocol_, rover_id_, nullptr, client_.get());

  std::cout << "[ROVER] Initialized with ID: '" << rover_id_
            << "', target base: " << base_host << ":" << base_port << std::endl;
}

// --- Destructor ---
Rover::~Rover() { stop(); }

// --- Start / Stop ---
void Rover::start() {
  bool expected = false;
  // Basic check if already started (though not fully thread-safe without atomic
  // state) This assumes start() is called from a single thread context setup
  // phase.

  if (message_manager_)
    message_manager_->start();
  if (protocol_)
    protocol_->start();
  protocol_->set_timeout_callback([this](const udp::endpoint &recipient) {
    // Post to io_context to avoid issues if callback is invoked
    // from a network thread internal to ReliabilityManager/Protocol.
    boost::asio::post(
        io_context_, [this, recipient]() { handle_packet_timeout(recipient); });
  });
  if (client_)
    client_->start_receive();

  // Setup callback from MessageManager (which gets data+sender from UdpClient)
  if (message_manager_) {
    message_manager_->set_message_callback(
        [this](std::unique_ptr<Message> message, const udp::endpoint &sender) {
          // Route all messages received through the standard pipeline
          route_message(std::move(message), sender);
        });
  } else {
    std::cerr
        << "[ROVER] Error: Cannot set message callback, MessageManager is null."
        << std::endl;
  }

  initiate_handshake(); // Start session with base station
  std::cout << "[ROVER] Started and initiating handshake." << std::endl;
}

void Rover::stop() {
  std::cout << "[ROVER] Stopping..." << std::endl; // Log start of stop sequence

  // Cancel timers first
  boost::system::error_code ec;
  status_timer_.cancel(ec);
  handshake_timer_.cancel(ec);
  position_telemetry_timer_.cancel(ec);
  probe_timer_.cancel(ec);

  // Stop layers in reverse order
  if (client_)
    client_->stop_receive(); // Stops UDP listener
  if (protocol_)
    protocol_->stop(); // Stops Lumen processing
  if (message_manager_)
    message_manager_->stop(); // Stops message handling

  // Reset state
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_state_ = SessionState::INACTIVE;
  }
  {
    std::lock_guard<std::mutex> lock(discovery_mutex_);
    discovered_rovers_.clear(); // Clear discovered rovers
  }
  {
    std::lock_guard<std::mutex> lock(stored_packets_mutex_);
    stored_packets_.clear();
  }
  std::cout << "[ROVER] Stopped" << std::endl;
}

void Rover::update_position(double lat, double lon) {
  std::lock_guard<std::mutex> lock(position_mutex_);
  current_latitude_ = lat;
  current_longitude_ = lon;

  std::cout << "[ROVER] Position updated: Lat=" << lat << ", Lon=" << lon
            << std::endl;
}

std::vector<uint8_t> string_to_binary(const std::string &str) {
  return std::vector<uint8_t>(str.begin(), str.end());
}

void Rover::store_message_for_later(const Message &message,
                                    const udp::endpoint &intended_recipient) {
  try {
    std::string json_str = message.serialise();
    std::vector<uint8_t> payload =
        string_to_binary(json_str); // Assuming static helper exists or move it
    LumenHeader::MessageType lumen_type = message.get_lumen_type();
    LumenHeader::Priority priority = message.get_lumen_priority();

    std::lock_guard<std::mutex> lock(stored_packets_mutex_);
    stored_packets_.emplace_back(std::move(payload), lumen_type, priority,
                                 intended_recipient);
    std::cout << "[ROVER] Stored message type " << message.get_type()
              << " for later sending (" << stored_packets_.size() << " stored)."
              << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[ROVER] Error serializing message for storage: " << e.what()
              << std::endl;
  }
}

// --- Callback Setters ---
void Rover::set_application_message_handler(ApplicationMessageHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  application_message_handler_ = std::move(handler);
  std::cout << "[ROVER] Application message handler registered." << std::endl;
}

void Rover::set_discovery_message_handler(DiscoveryMessageHandler handler) {
  std::lock_guard<std::mutex> lock(discovery_handler_mutex_);
  discovery_message_handler_ = std::move(handler);
  std::cout << "[ROVER] Discovery message handler registered." << std::endl;
}

void Rover::send_telemetry(const std::map<std::string, double> &readings) {
  SessionState current_state;
  udp::endpoint base_ep;
  bool target_is_base = false;

  try {
    base_ep = get_base_endpoint();
    target_is_base = true; // Telemetry always goes to base
  } catch (const std::runtime_error &e) {
    std::cerr << "[ROVER] Error getting base endpoint for telemetry: "
              << e.what() << std::endl;
    return; // Cannot determine target
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  TelemetryMessage telemetry_msg(readings, rover_id_);

  if (current_state == SessionState::DISCONNECTED && target_is_base) {
    store_message_for_later(telemetry_msg, base_ep);
  } else if (current_state == SessionState::ACTIVE && target_is_base) {
    if (message_manager_) {
      message_manager_->send_message(telemetry_msg, base_ep);
    } else {
      std::cerr
          << "[ROVER] Error: Cannot send telemetry, MessageManager is null."
          << std::endl;
    }
  } else if (!target_is_base) {
    // Logic for sending telemetry to non-base (if ever needed)
    std::cerr
        << "[ROVER] Warning: Telemetry sending to non-base not implemented."
        << std::endl;
  } else {
    std::cout << "[ROVER] Cannot send telemetry, session not active or "
                 "disconnected targetting non-base."
              << std::endl;
  }
}

void Rover::send_message(const Message &message,
                         const udp::endpoint &recipient) {
  SessionState current_state;
  udp::endpoint target_endpoint = recipient;
  bool target_is_base = false;
  udp::endpoint base_ep_check;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  // Determine final target and if it's the base station
  try {
    base_ep_check = get_base_endpoint();
    if (target_endpoint.address().is_unspecified() ||
        target_endpoint == base_ep_check) {
      target_endpoint = base_ep_check; // Default or explicit base target
      target_is_base = true;
    }
  } catch (const std::runtime_error &e) {
    std::cerr << "[ROVER] Error checking base endpoint for send_message: "
              << e.what() << std::endl;
    // If base isn't registered, we can't default to it. Only proceed if
    // recipient was explicit.
    if (target_endpoint.address().is_unspecified()) {
      std::cout << "[ROVER] Cannot send message type " << message.get_type()
                << ": Base endpoint unknown and no specific recipient."
                << std::endl;
      return;
    }
    target_is_base = false; // Assume not base if we can't resolve it
  }
  if (current_state == SessionState::DISCONNECTED && target_is_base) {
    store_message_for_later(message, target_endpoint);
  } else if (current_state == SessionState::ACTIVE || !target_is_base) {
    // Send if active OR if sending to non-base endpoint (allowed even if
    // disconnected from base)
    if (message_manager_) {
      message_manager_->send_message(message, target_endpoint);
      std::cout << "[ROVER] Sent message type: " << message.get_type()
                << " via protocol to " << endpoint_to_string(target_endpoint)
                << std::endl;
    } else {
      std::cerr << "[ROVER] Error: Cannot send message type "
                << message.get_type() << ", MessageManager is null."
                << std::endl;
    }
  } else {
    std::cout << "[ROVER] Cannot send message type " << message.get_type()
              << " to base. Session state: " << static_cast<int>(current_state)
              << std::endl;
  }
}

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
  std::cout << "[ROVER] Sent raw (JSON) message type: " << message.get_type()
            << " to " << endpoint_to_string(recipient) << std::endl;
}

// --- Status Methods ---
void Rover::update_status(StatusMessage::StatusLevel level,
                          const std::string &description) {
  std::lock_guard<std::mutex> lock(status_mutex_);
  current_status_level_ = level;
  current_status_description_ = description;
}

void Rover::send_status() {
  StatusMessage::StatusLevel current_level;
  std::string current_desc;
  SessionState current_state;
  udp::endpoint base_ep;
  bool target_is_base = false;

  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    current_level = current_status_level_;
    current_desc = current_status_description_;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  try {
    base_ep = get_base_endpoint();
    target_is_base = true; // Status always goes to base
  } catch (const std::runtime_error &e) {
    std::cerr << "[ROVER] Error getting base endpoint for status: " << e.what()
              << std::endl;
    return; // Cannot determine target
  }

  StatusMessage status_msg(current_level, current_desc, rover_id_);

  if (current_state == SessionState::DISCONNECTED && target_is_base) {
    store_message_for_later(status_msg, base_ep);
  } else if (current_state == SessionState::ACTIVE && target_is_base) {
    if (message_manager_) {
      message_manager_->send_message(status_msg, base_ep);
    } else {
      std::cerr << "[ROVER] Error: Cannot send status, MessageManager is null."
                << std::endl;
    }
  } else {
    std::cout << "[ROVER] Cannot send status, session not active." << std::endl;
  }
}

// --- Discovery Methods ---
void Rover::scan_for_rovers(int discovery_port, const std::string &message) {
  if (!client_) {
    std::cerr << "[ROVER] Error: Cannot scan, UdpClient is null." << std::endl;
    return;
  }

  try {
    std::cout << "[ROVER] Sending unicast scan (" << message
              << ") to subnet 10.237.0.0/24 on port " << discovery_port
              << std::endl;

    CommandMessage discover_msg("ROVER_DISCOVER", message, rover_id_);
    std::string json_payload = discover_msg.serialise();
    std::vector<uint8_t> data_to_send(json_payload.begin(), json_payload.end());

    for (int i = 2; i <= 150; ++i) {
      std::string ip_str = "10.237.0." + std::to_string(i);

      boost::system::error_code ec;
      boost::asio::ip::address_v4 target_addr =
          boost::asio::ip::make_address_v4(ip_str, ec);

      if (ec) {
        std::cerr << "[ROVER] Error creating address " << ip_str << ": "
                  << ec.message() << std::endl;
        continue;
      }

      udp::endpoint target_endpoint(
          target_addr, static_cast<unsigned short>(discovery_port));
      client_->send_data_to(data_to_send, target_endpoint);
    }

    std::cout << "[ROVER] Finished sending unicast scan packets." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[ROVER] Error during unicast scan: " << e.what() << std::endl;
  }
}

std::map<std::string, udp::endpoint> Rover::get_discovered_rovers() const {
  std::lock_guard<std::mutex> lock(discovery_mutex_);
  return discovered_rovers_; // Return a copy
}

void Rover::handle_discovery_command(CommandMessage *cmd_msg,
                                     const udp::endpoint &sender) {
  if (!cmd_msg)
    return;

  const std::string &command = cmd_msg->get_command();
  const std::string &sender_id =
      cmd_msg->get_sender(); // ID of the rover sending the command

  // Ignore messages from self
  if (sender_id == rover_id_) {
    return;
  }

  std::cout << "[ROVER DISCOVERY] Handling discovery command '" << command
            << "' from " << sender_id << " at " << endpoint_to_string(sender)
            << std::endl;

  // Handle an announcement from another rover
  if (command == "ROVER_ANNOUNCE") {
    bool is_new = false;
    {
      std::lock_guard<std::mutex> lock(discovery_mutex_);
      auto [iter, inserted] =
          discovered_rovers_.insert_or_assign(sender_id, sender);
      is_new = inserted;
    }

    if (is_new) {
      std::cout << "[ROVER DISCOVERY] Discovered new rover: " << sender_id
                << " at " << endpoint_to_string(sender) << std::endl;

      // Notify the application layer via its callback
      DiscoveryMessageHandler handler_copy;
      {
        std::lock_guard<std::mutex> lock(discovery_handler_mutex_);
        handler_copy = discovery_message_handler_;
      }
      if (handler_copy) {
        boost::asio::post(io_context_, [handler = std::move(handler_copy),
                                        id = sender_id, ep = sender]() {
          try {
            handler(id, ep);
          } catch (const std::exception &e) {
            std::cerr
                << "[ERROR] Rover: Exception in discovery message handler: "
                << e.what() << std::endl;
          }
        });
      }
    } else {
      std::cout << "[ROVER DISCOVERY] Rover " << sender_id
                << " announced again from " << endpoint_to_string(sender) << "."
                << std::endl;
    }
  }
  // Handle receiving a discovery probe from another rover
  else if (command == "ROVER_DISCOVER") {
    std::cout << "[ROVER DISCOVERY] Received discovery probe from " << sender_id
              << ". Sending announcement." << std::endl;
    // Send our own announcement back directly to the sender using a raw message
    CommandMessage announce_msg("ROVER_ANNOUNCE", "Responding to probe",
                                rover_id_);
    send_raw_message(announce_msg, sender);
  }
}

// --- Routing and Internal Command Handling ---
void Rover::route_message(std::unique_ptr<Message> message,
                          const udp::endpoint &sender) {
  if (!message) {
    std::cerr
        << "[ROVER] Error: Received null message pointer in route_message."
        << std::endl;
    return;
  }

  std::string msg_type = message->get_type();
  std::string sender_id = message->get_sender();

  if (sender_id == rover_id_) {
    std::cout << "[ROVER INTERNAL] Ignoring message type: '" << msg_type
              << "' from self (ID: '" << sender_id << "')." << std::endl;
    return; // Stop processing self-sent messages
  }

  bool from_base_station = false;
  try {
    // Check if sender matches the registered base endpoint
    from_base_station = (client_ && sender == client_->get_base_endpoint());
  } catch (const std::runtime_error &e) {
    std::cerr
        << "[ROVER] Warning: Could not get base endpoint in route_message: "
        << e.what() << std::endl;
    // Proceed assuming not from base if endpoint isn't registered/available
  }

  std::cout << "[ROVER INTERNAL] Routing message type: '" << msg_type
            << "' from sender ID: '" << sender_id << "' at "
            << endpoint_to_string(sender)
            << (from_base_station ? " (Base Station)" : " (Other)")
            << std::endl;

  // --- Internal Handling ---
  if (msg_type == CommandMessage::message_type()) {
    auto *cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    if (cmd_msg) {
      const std::string &command = cmd_msg->get_command();

      // Session commands (must be from Base Station)
      if (command == "SESSION_ACCEPT" || command == "SESSION_ESTABLISHED") {
        if (from_base_station) {
          handle_internal_command(cmd_msg, sender);
        } else {
          std::cerr << "[ROVER] Warning: Ignoring session command '" << command
                    << "' from non-base endpoint: "
                    << endpoint_to_string(sender) << std::endl;
        }
        return; // Session commands fully handled
      }
      // Discovery commands (can be from any rover, including self if broadcast
      // loopback occurs)
      else if (command == "ROVER_ANNOUNCE" || command == "ROVER_DISCOVER") {
        // Check if message is from self (can happen with broadcast)
        if (sender_id != rover_id_) {
          handle_discovery_command(cmd_msg, sender);
        } else {
          std::cout << "[ROVER INTERNAL] Ignoring self-discovery message."
                    << std::endl;
        }
        return; // Discovery commands fully handled
      }
      // Other potential internal commands could go here
    }
  }

  // --- Application Handling ---
  // If not handled internally, pass to the application handler
  ApplicationMessageHandler handler_copy;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler_copy = application_message_handler_;
  }

  if (handler_copy) {
    // Post to io_context to avoid blocking network thread
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
    // Log if no handler exists for non-internal messages
    // (Excluding potential background noise like ACKs/NAKs if LumenProtocol
    // handled them lower down)
    std::cout << "[ROVER INTERNAL] No application handler registered for "
                 "message type: '"
              << msg_type << "' from sender ID '" << sender_id << "'."
              << std::endl;
  }
}

void Rover::handle_internal_command(CommandMessage *cmd_msg,
                                    const udp::endpoint &sender) {
  if (!cmd_msg)
    return;
  const std::string &command = cmd_msg->get_command();

  if (command == "SESSION_ACCEPT") {
    handle_session_accept();
  } else if (command == "SESSION_ESTABLISHED") {
    handle_session_established();
  }
}

void Rover::initiate_handshake() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (session_state_ != SessionState::INACTIVE &&
        session_state_ != SessionState::DISCONNECTED) {
      std::cout << "[ROVER INTERNAL] Handshake initiation skipped, state is "
                   "not INACTIVE or DISCONNECTED ("
                << static_cast<int>(session_state_) << ")" << std::endl;
      return;
    }
    // If disconnected, cancel the probe timer before starting handshake
    if (session_state_ == SessionState::DISCONNECTED) {
      boost::system::error_code ec;
      probe_timer_.cancel(ec);
      std::cout << "[ROVER INTERNAL] Cancelling probe timer due to handshake "
                   "initiation."
                << std::endl;
    }

    session_state_ = SessionState::HANDSHAKE_INIT;
    handshake_retry_count_ = 0;
  }

  send_command("SESSION_INIT", rover_id_); // Send INIT via normal protocol path
  std::cout << "[ROVER INTERNAL] Sent SESSION_INIT." << std::endl;

  // Start the handshake timer
  boost::system::error_code ec;
  handshake_timer_.expires_after(HANDSHAKE_TIMEOUT);
  handshake_timer_.async_wait([this](const boost::system::error_code &ec) {
    if (!ec) {
      handle_handshake_timer();
    } else if (ec != boost::asio::error::operation_aborted) {
      std::cerr << "[ROVER INTERNAL] Handshake timer error: " << ec.message()
                << std::endl;
    }
  });
}

void Rover::handle_handshake_timer() {
  SessionState current_state;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  if (current_state == SessionState::HANDSHAKE_INIT ||
      current_state == SessionState::HANDSHAKE_ACCEPT) {
    if (handshake_retry_count_ < MAX_HANDSHAKE_RETRIES) {
      handshake_retry_count_++;
      std::cout << "[ROVER INTERNAL] Handshake timeout. Retrying (attempt "
                << handshake_retry_count_ << "/" << MAX_HANDSHAKE_RETRIES
                << ")..." << std::endl;

      if (current_state == SessionState::HANDSHAKE_INIT) {
        send_command("SESSION_INIT", rover_id_);
      } else { // HANDSHAKE_ACCEPT
        send_command("SESSION_CONFIRM", rover_id_);
      }

      // Reschedule timer
      boost::system::error_code ec;
      handshake_timer_.expires_after(HANDSHAKE_TIMEOUT);
      handshake_timer_.async_wait([this](const boost::system::error_code &ec) {
        if (!ec) {
          handle_handshake_timer();
        }
      });
    } else {
      std::cerr << "[ROVER INTERNAL] Handshake failed after "
                << MAX_HANDSHAKE_RETRIES << " attempts.." << std::endl;
      std::cerr << "[ROVER INTERNAL] Setting connection status to disconnected."
                << std::endl;
      handle_base_disconnect();
    }
  }
}

void Rover::send_stored_packets() {
  std::lock_guard<std::mutex> lock(stored_packets_mutex_);
  if (stored_packets_.empty()) {
    return;
  }

  std::cout << "[ROVER INTERNAL] Sending " << stored_packets_.size()
            << " stored packets..." << std::endl;

  if (!protocol_) {
    std::cerr << "[ROVER INTERNAL] Error: Cannot send stored packets, "
                 "LumenProtocol is null."
              << std::endl;
    stored_packets_.clear(); // Clear queue as we cannot send them
    return;
  }

  // Send all stored packets using structured bindings
  while (!stored_packets_.empty()) {
    auto [payload, type, priority, recipient] =
        stored_packets_.front(); // Deconstruct the tuple

    try {
      protocol_->send_message(payload, type, priority, recipient);

      std::cout << "[ROVER INTERNAL] Sent stored packet type "
                << static_cast<int>(static_cast<uint8_t>(type)) << " to "
                << recipient << std::endl;

    } catch (const std::exception &e) {
      std::cerr << "[ROVER INTERNAL] Error sending stored packet: " << e.what()
                << std::endl;
    }
    stored_packets_.pop_front(); // Remove sent/failed packet from queue
  }
  std::cout << "[ROVER INTERNAL] Finished sending stored packets." << std::endl;
}

void Rover::handle_session_established() {
  std::cout << "[ROVER INTERNAL] Received SESSION_ESTABLISHED." << std::endl;
  bool needs_setup = false;
  SessionState previous_state;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    previous_state = session_state_;
    if (session_state_ != SessionState::ACTIVE) {
      if (session_state_ != SessionState::HANDSHAKE_ACCEPT) {
        std::cout << "[ROVER INTERNAL] Warning: Received SESSION_ESTABLISHED "
                     "in unexpected state: "
                  << static_cast<int>(session_state_) << std::endl;
      }

      session_state_ = SessionState::ACTIVE;
      handshake_retry_count_ = 0;
      needs_setup = true;
      std::cout << "[ROVER INTERNAL] Session ACTIVE." << std::endl;

      boost::system::error_code ec;
      handshake_timer_.cancel(ec);
      probe_timer_.cancel(ec);
    } else {
      std::cout << "[ROVER INTERNAL] Ignoring SESSION_ESTABLISHED, session "
                   "already ACTIVE."
                << std::endl;
    }
  }

  if (needs_setup) {

    send_stored_packets();

    std::cout
        << "[ROVER INTERNAL] Starting periodic status and telemetry timers."
        << std::endl;
    handle_status_timer();
    handle_position_telemetry_timer();
  }
}

void Rover::handle_position_telemetry_timer() {
  double lat, lon;
  SessionState current_state;

  {
    std::lock_guard<std::mutex> lock(position_mutex_);
    lat = current_latitude_;
    lon = current_longitude_;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  // Attempt to send telemetry. If DISCONNECTED, send_telemetry will queue it.
  std::map<std::string, double> position_data = {{"latitude", lat},
                                                 {"longitude", lon}};
  send_telemetry(position_data);

  // Always reschedule the timer as long as the rover is running
  // The sending logic inside send_telemetry handles the state (ACTIVE vs
  // DISCONNECTED).
  boost::system::error_code ec;
  position_telemetry_timer_.expires_after(POSITION_TELEMETRY_INTERVAL);
  position_telemetry_timer_.async_wait(
      [this](const boost::system::error_code &ec) {
        if (!ec) {
          handle_position_telemetry_timer(); // Reschedule regardless of state
        } else if (ec != boost::asio::error::operation_aborted) {
          std::cerr << "[ROVER INTERNAL] Position telemetry timer wait error: "
                    << ec.message() << std::endl;
        } else {
          std::cout
              << "[ROVER INTERNAL] Position telemetry timer stopped (aborted)."
              << std::endl;
        }
      });

  if (current_state == SessionState::DISCONNECTED) {
    std::cout
        << "[ROVER INTERNAL] Position telemetry timer ran (DISCONNECTED state)."
        << std::endl;
  }
}

void Rover::handle_status_timer() {
  SessionState current_state;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  // Attempt to send status. If DISCONNECTED, send_status will queue it.
  send_status();

  // Always reschedule the timer as long as the rover is running
  // The sending logic inside send_status handles the state (ACTIVE vs
  // DISCONNECTED).
  boost::system::error_code ec;
  status_timer_.expires_after(STATUS_INTERVAL);
  status_timer_.async_wait([this](const boost::system::error_code &ec) {
    if (!ec) {               // If no error (like timer cancellation)
      handle_status_timer(); // Reschedule regardless of state
    } else if (ec != boost::asio::error::operation_aborted) {
      std::cerr << "[ROVER INTERNAL] Status timer wait error: " << ec.message()
                << std::endl;

    } else {
      std::cout << "[ROVER INTERNAL] Status timer stopped (aborted)."
                << std::endl;
    }
  });

  if (current_state == SessionState::DISCONNECTED) {
    std::cout << "[ROVER INTERNAL] Status timer ran (DISCONNECTED state)."
              << std::endl;
  }
}

// --- Getters ---
Rover::SessionState Rover::get_session_state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return session_state_;
}

const udp::endpoint &Rover::get_base_endpoint() const {
  if (!client_) {
    throw std::runtime_error("[ROVER] Error: UdpClient is not initialized.");
  }
  // get_base_endpoint() in UdpClient already throws if not registered
  return client_->get_base_endpoint();
}

void Rover::send_command(const std::string &command,
                         const std::string &params) {
  if (!message_manager_) {
    std::cerr << "[ROVER] Error: Cannot send command '" << command
              << "', MessageManager is null." << std::endl;
    return;
  }
  CommandMessage cmd_msg(command, params, rover_id_);
  udp::endpoint base_ep;
  try {
    base_ep = get_base_endpoint();
  } catch (const std::runtime_error &e) {
    std::cerr << "[ROVER] Error getting base endpoint for send_command '"
              << command << "': " << e.what() << std::endl;
    return;
  }

  SessionState current_state;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  if (current_state == SessionState::DISCONNECTED &&
      command == "SESSION_INIT") {
    // Allow sending the probe even when disconnected
    message_manager_->send_message(cmd_msg, base_ep);
    std::cout << "[ROVER] Sent SESSION_INIT probe while disconnected."
              << std::endl;
  } else if (current_state != SessionState::DISCONNECTED) {
    // Send normally if not disconnected
    message_manager_->send_message(cmd_msg, base_ep);
  } else {
    // Do not send other commands while disconnected
    std::cout << "[ROVER] Suppressed sending command '" << command
              << "' while disconnected." << std::endl;
  }
}

void Rover::handle_base_disconnect() {
  // Use a lock to ensure state is checked and modified atomically
  std::lock_guard<std::mutex> lock(state_mutex_);

  // Check if already disconnected or inactive to avoid redundant actions
  if (session_state_ != SessionState::ACTIVE) {
    std::cout << "[ROVER INTERNAL] Ignoring disconnect signal, not in ACTIVE "
                 "state (current state: "
              << static_cast<int>(session_state_) << ")." << std::endl;
    return; // Already disconnected or never connected
  }

  std::cout << "[ROVER INTERNAL] Detected base station disconnect. "
               "Transitioning to DISCONNECTED state."
            << std::endl;
  session_state_ = SessionState::DISCONNECTED;

  // Cancel timers that depend on ACTIVE state
  boost::system::error_code ec;
  status_timer_.cancel(ec);
  position_telemetry_timer_.cancel(ec);
  handshake_timer_.cancel(ec);
  probe_timer_.cancel(ec); // Cancel any existing probe timer first

  // Reset protocol sequence number and reliability state
  // Post the reset to the io_context to run safely on the event loop thread
  boost::asio::post(io_context_, [this]() {
    if (protocol_) { // Check pointer validity inside the handler
      protocol_->reset_sequence_number();
    } else {
      std::cerr << "[ROVER INTERNAL] Warning: Cannot reset sequence number, "
                   "protocol_ is null."
                << std::endl;
    }
  });

  // Start the probe timer (also post to ensure it runs on the event loop)
  boost::asio::post(io_context_, [this]() {
    probe_timer_.expires_after(std::chrono::seconds(0)); // Start immediately
    probe_timer_.async_wait([this](const boost::system::error_code &error) {
      if (!error) {
        handle_probe_timer(); // schedule next probe check
      } else if (error != boost::asio::error::operation_aborted) {
        std::cerr << "[ROVER INTERNAL] Probe timer wait error: "
                  << error.message() << std::endl;
      }
    });
    std::cout << "[ROVER INTERNAL] Started periodic probing timer."
              << std::endl;
  });
}

void Rover::handle_probe_timer() {
  SessionState current_state;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  // Only probe if still in disconnected state
  if (current_state == SessionState::DISCONNECTED) {
    std::cout
        << "[ROVER INTERNAL] Sending probe (SESSION_INIT) to base station..."
        << std::endl;
    send_command("SESSION_INIT", rover_id_); // Send probe

    // Reschedule the timer
    probe_timer_.expires_at(probe_timer_.expiry() + PROBE_INTERVAL);
    probe_timer_.async_wait([this](const boost::system::error_code &error) {
      if (!error) {
        handle_probe_timer(); // Continue probing
      } else if (error != boost::asio::error::operation_aborted) {
        std::cerr << "[ROVER INTERNAL] Probe timer error on reschedule: "
                  << error.message() << std::endl;
      } else {
        std::cout << "[ROVER INTERNAL] Probe timer cancelled." << std::endl;
      }
    });
  } else {
    std::cout << "[ROVER INTERNAL] Probe timer fired but no longer "
                 "disconnected. Stopping probe."
              << std::endl;
    // Do not reschedule if state changed (e.g., handshake started)
  }
}

void Rover::handle_packet_timeout(const udp::endpoint &recipient) {
  SessionState current_state;
  udp::endpoint base_ep;
  bool is_base_target = false;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  try {
    base_ep = get_base_endpoint();
    is_base_target = (recipient == base_ep);
  } catch (const std::runtime_error &e) {
    std::cerr
        << "[ROVER] Error getting base endpoint in handle_packet_timeout: "
        << e.what() << std::endl;
    is_base_target = false;
  }

  // Only trigger disconnect logic if we were ACTIVE and the timeout was for the
  // base station
  if (current_state == SessionState::ACTIVE && is_base_target) {
    std::cout
        << "[ROVER INTERNAL] Received timeout notification for base station."
        << std::endl;
    handle_base_disconnect(); // Let ReliabilityManager trigger this via
                              // callback
  } else if (is_base_target) {
    std::cout << "[ROVER INTERNAL] Received timeout notification for base "
                 "station but not in ACTIVE state ("
              << static_cast<int>(current_state) << "). Ignoring." << std::endl;
  } else {

    std::cout << "[ROVER INTERNAL] Received timeout notification for non-base "
                 "recipient: "
              << recipient << ". Ignoring." << std::endl;
  }
}

void Rover::handle_session_accept() {
  std::cout << "[ROVER INTERNAL] Received SESSION_ACCEPT." << std::endl;
  bool state_updated = false;
  SessionState previous_state;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    previous_state = session_state_; // Record state before changing

    // Allow transition from HANDSHAKE_INIT or DISCONNECTED (if probe received
    // ACCEPT)
    if (session_state_ == SessionState::HANDSHAKE_INIT ||
        session_state_ == SessionState::DISCONNECTED) {
      session_state_ = SessionState::HANDSHAKE_ACCEPT;
      state_updated = true;
      std::cout << "[ROVER INTERNAL] State updated to HANDSHAKE_ACCEPT (from "
                << (previous_state == SessionState::DISCONNECTED
                        ? "DISCONNECTED"
                        : "HANDSHAKE_INIT")
                << ")." << std::endl;
    } else {
      std::cout << "[ROVER INTERNAL] Ignoring SESSION_ACCEPT received in "
                   "unexpected state: "
                << static_cast<int>(session_state_) << std::endl;
    }
  } // Lock released

  if (state_updated) {
    // Cancel timers associated with previous states (INIT or DISCONNECTED
    // probing)
    boost::system::error_code ec_handshake, ec_probe;
    handshake_timer_.cancel(
        ec_handshake); // Cancel handshake retry timer if it was running

    // Crucially, cancel the probe timer if we were in the DISCONNECTED state
    if (previous_state == SessionState::DISCONNECTED) {
      probe_timer_.cancel(ec_probe);
      if (!ec_probe || ec_probe == boost::asio::error::operation_aborted) {
        std::cout << "[ROVER INTERNAL] Canceled probe timer upon receiving "
                     "SESSION_ACCEPT."
                  << std::endl;
      } else {
        std::cerr << "[ROVER INTERNAL] Error cancelling probe timer: "
                  << ec_probe.message() << std::endl;
      }
    }

    // Proceed to confirm the session
    send_command("SESSION_CONFIRM", rover_id_);
    std::cout << "[ROVER INTERNAL] Sent SESSION_CONFIRM." << std::endl;

    // Re-arm handshake timer to wait for SESSION_ESTABLISHED
    // Ensure this runs safely on the io_context thread
    boost::asio::post(io_context_, [this]() {
      handshake_timer_.expires_after(HANDSHAKE_TIMEOUT);
      handshake_timer_.async_wait([this](const boost::system::error_code &ec) {
        if (!ec) {
          handle_handshake_timer(); // Handle timeout waiting for ESTABLISHED
        } else if (ec != boost::asio::error::operation_aborted) {
          std::cerr
              << "[ROVER INTERNAL] Handshake timer (wait ESTABLISHED) error: "
              << ec.message() << std::endl;
        }
      });
    });
  }
}