// src/rover/rover.cpp

#include "rover.hpp"
#include "command_message.hpp"
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
      handshake_timer_(io_context), session_state_(SessionState::INACTIVE),
      rover_id_(rover_id),
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
  std::cout << "[ROVER] Stopped" << std::endl;
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

// --- Send Methods ---
void Rover::send_telemetry(const std::map<std::string, double> &readings) {
  bool can_send = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    can_send = (session_state_ == SessionState::ACTIVE);
  }

  if (can_send) {
    TelemetryMessage telemetry_msg(readings, rover_id_);
    if (message_manager_) {
      // Send telemetry only to the base station by default
      udp::endpoint base_ep;
      try {
        base_ep = get_base_endpoint();
      } catch (const std::runtime_error &e) {
        std::cerr << "[ROVER] Error getting base endpoint for telemetry: "
                  << e.what() << std::endl;
        return;
      }
      message_manager_->send_message(telemetry_msg, base_ep);
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

void Rover::send_message(const Message &message,
                         const udp::endpoint &recipient) {
  bool is_session_active = false;
  udp::endpoint target_endpoint = recipient;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    is_session_active = (session_state_ == SessionState::ACTIVE);
  }

  if (target_endpoint.address()
          .is_unspecified()) { // No specific recipient provided
    if (is_session_active && client_) {
      try {
        target_endpoint = client_->get_base_endpoint();
      } catch (const std::runtime_error &e) {
        std::cerr << "[ROVER] Error getting base endpoint for send_message: "
                  << e.what() << std::endl;
        return; // Cannot determine target
      }
    } else {
      std::cout << "[ROVER] Cannot send message type " << message.get_type()
                << ": No recipient specified and session not active."
                << std::endl;
      return;
    }
  }

  // Send to the determined target_endpoint
  if (message_manager_) {
    message_manager_->send_message(message, target_endpoint);
    std::cout << "[ROVER] Sent message type: " << message.get_type()
              << " via protocol to " << endpoint_to_string(target_endpoint)
              << std::endl; // More detailed log
  } else {
    std::cerr << "[ROVER] Error: Cannot send message type "
              << message.get_type() << ", MessageManager is null." << std::endl;
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
  bool can_send = false;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    current_level = current_status_level_;
    current_desc = current_status_description_;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    can_send = (session_state_ == SessionState::ACTIVE);
  }

  if (can_send) {
    StatusMessage status_msg(current_level, current_desc, rover_id_);
    if (message_manager_) {
      udp::endpoint base_ep;
      try {
        base_ep = get_base_endpoint();
      } catch (const std::runtime_error &e) {
        std::cerr << "[ROVER] Error getting base endpoint for status: "
                  << e.what() << std::endl;
        return;
      }
      message_manager_->send_message(status_msg,
                                     base_ep); // Send status only to base
    } else {
      std::cerr << "[ROVER] Error: Cannot send status, MessageManager is null."
                << std::endl;
    }
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

// --- Session Management (Implementations largely unchanged) ---
void Rover::initiate_handshake() {
  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (session_state_ != SessionState::INACTIVE) {
      std::cout << "[ROVER INTERNAL] Handshake initiation skipped, state is "
                   "not INACTIVE ("
                << static_cast<int>(session_state_) << ")" << std::endl;
      return;
    }
    session_state_ = SessionState::HANDSHAKE_INIT;
    handshake_retry_count_ = 0;
  } // State Mutex Lock Released

  send_command("SESSION_INIT", rover_id_);
  std::cout << "[ROVER INTERNAL] Sent SESSION_INIT." << std::endl;

  // Start the handshake timer
  boost::system::error_code ec;
  handshake_timer_.expires_after(HANDSHAKE_TIMEOUT);
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
                << MAX_HANDSHAKE_RETRIES << " attempts. Resetting to INACTIVE."
                << std::endl;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        session_state_ = SessionState::INACTIVE;
      }
    }
  }
}

void Rover::handle_session_accept() {
  std::cout << "[ROVER INTERNAL] Received SESSION_ACCEPT." << std::endl;
  bool state_updated = false;
  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);
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
    send_command("SESSION_CONFIRM", rover_id_);
    std::cout << "[ROVER INTERNAL] Sent SESSION_CONFIRM." << std::endl;
    // Cancel the timer now that we've sent confirm, wait for ESTABLISHED
    boost::system::error_code ec;
    handshake_timer_.cancel(ec);
  }
}

void Rover::handle_session_established() {
  std::cout << "[ROVER INTERNAL] Received SESSION_ESTABLISHED." << std::endl;
  bool needs_status_timer_start = false;
  { // State Mutex Lock Scope
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (session_state_ != SessionState::ACTIVE) {
      if (session_state_ != SessionState::HANDSHAKE_ACCEPT) {
        std::cout << "[ROVER INTERNAL] Warning: Received SESSION_ESTABLISHED "
                     "in unexpected state: "
                  << static_cast<int>(session_state_) << std::endl;
      }
      session_state_ = SessionState::ACTIVE;
      handshake_retry_count_ = 0; // Reset retries on success
      needs_status_timer_start = true;
      std::cout << "[ROVER INTERNAL] Session ACTIVE." << std::endl;

      // Cancel handshake timer
      boost::system::error_code ec;
      handshake_timer_.cancel(ec);
    } else {
      std::cout << "[ROVER INTERNAL] Ignoring SESSION_ESTABLISHED, session "
                   "already ACTIVE."
                << std::endl;
    }
  } // State Mutex Lock Released

  if (needs_status_timer_start) {
    std::cout << "[ROVER INTERNAL] Starting periodic status timer."
              << std::endl;
    handle_status_timer(); // Start the timer loop
  }
}

// --- Timer Handlers ---
void Rover::handle_status_timer() {
  send_status(); // Attempt to send status

  bool is_active;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    is_active = (session_state_ == SessionState::ACTIVE);
  }

  if (is_active) {
    boost::system::error_code ec;
    status_timer_.expires_after(STATUS_INTERVAL);
    status_timer_.async_wait(
        [this, &is_active](const boost::system::error_code &ec) {
          // Check error code and if still active before recursing
          if (!ec && get_session_state() == SessionState::ACTIVE) {
            handle_status_timer();
          } else if (ec && ec != boost::asio::error::operation_aborted) {
            std::cerr << "[ROVER INTERNAL] Status timer wait error: "
                      << ec.message() << std::endl;
          } else if (!is_active) {
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

// --- Internal Send Command ---
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
    base_ep = get_base_endpoint(); // Send session commands only to base
    message_manager_->send_message(cmd_msg, base_ep);
  } catch (const std::runtime_error &e) {
    std::cerr << "[ROVER] Error getting base endpoint for send_command '"
              << command << "': " << e.what() << std::endl;
    return;
  }
}