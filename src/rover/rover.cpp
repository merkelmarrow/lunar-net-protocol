// src/rover/rover.cpp

#include "rover.hpp"
#include "basic_message.hpp"
#include "command_message.hpp"
#include "lumen_protocol.hpp"
#include "message_manager.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"
#include <boost/asio/post.hpp>
#include <chrono>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>

namespace { // internal linkage helpers

double calculate_distance(double x1, double y1, double x2, double y2) {
  double dx = x2 - x1;
  double dy = y2 - y1;
  return std::sqrt(dx * dx + dy * dy);
}

std::string endpoint_to_string(const udp::endpoint &ep) {
  boost::system::error_code ec;
  std::string addr = ep.address().to_string(ec);
  if (ec) {
    addr = "invalid_address";
  }
  return addr + ":" + std::to_string(ep.port());
}
} // anonymous namespace

// --- constructor ---
Rover::Rover(boost::asio::io_context &io_context, const std::string &base_host,
             int base_port, const std::string &rover_id)
    : io_context_(io_context), status_timer_(io_context),
      position_telemetry_timer_(io_context), handshake_timer_(io_context),
      probe_timer_(io_context), movement_timer_(io_context),
      session_state_(SessionState::INACTIVE), rover_id_(rover_id),
      current_status_level_(StatusMessage::StatusLevel::OK),
      current_status_description_("System OK"), handshake_retry_count_(0),
      low_power_mode_(false), target_coordinate_(std::nullopt),
      current_coordinate_({0.0, 0.0}) {
  // initialize communication layers
  client_ = std::make_unique<UdpClient>(io_context);
  try {
    client_->register_base(base_host, base_port);
  } catch (const std::exception &e) {
    std::cerr << "[ROVER] CRITICAL ERROR during initialization: Failed to "
                 "register base station at "
              << base_host << ":" << base_port << ". Error: " << e.what()
              << std::endl;
    throw; // propagate error
  }
  protocol_ = std::make_unique<LumenProtocol>(io_context, *client_);
  message_manager_ = std::make_unique<MessageManager>(
      io_context, *protocol_, rover_id_, nullptr, client_.get());

  std::cout << "[ROVER] Initialized with ID: '" << rover_id_
            << "', target base: " << base_host << ":" << base_port << std::endl;
}

// --- destructor ---
Rover::~Rover() { stop(); }

// --- start / stop ---
void Rover::start() {
  bool expected = false;
  // basic check if already started (assumes single-thread setup context)

  if (message_manager_)
    message_manager_->start();
  if (protocol_)
    protocol_->start();
  protocol_->set_timeout_callback([this](const udp::endpoint &recipient) {
    // post to io_context to avoid issues if callback invoked from internal
    // network thread
    boost::asio::post(
        io_context_, [this, recipient]() { handle_packet_timeout(recipient); });
  });
  if (client_)
    client_->start_receive();

  // setup callback from messagemanager
  if (message_manager_) {
    message_manager_->set_message_callback(
        [this](std::unique_ptr<Message> message, const udp::endpoint &sender) {
          // route all received messages through standard pipeline
          route_message(std::move(message), sender);
        });
  } else {
    std::cerr
        << "[ROVER] Error: Cannot set message callback, MessageManager is null."
        << std::endl;
  }

  initiate_handshake(); // start session with base station
  std::cout << "[ROVER] Started and initiating handshake." << std::endl;
}

void Rover::stop() {
  std::cout << "[ROVER] Stopping..." << std::endl;

  // cancel timers first
  boost::system::error_code ec;
  status_timer_.cancel(ec);
  handshake_timer_.cancel(ec);
  position_telemetry_timer_.cancel(ec);
  probe_timer_.cancel(ec);
  movement_timer_.cancel(ec);

  // stop layers in reverse order
  if (client_)
    client_->stop_receive();
  if (protocol_)
    protocol_->stop();
  if (message_manager_)
    message_manager_->stop();

  // reset state
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_state_ = SessionState::INACTIVE;
  }
  {
    std::lock_guard<std::mutex> lock(discovery_mutex_);
    discovered_rovers_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(stored_packets_mutex_);
    stored_packets_.clear();
  }
  std::cout << "[ROVER] Stopped" << std::endl;
}

void Rover::update_current_position(double lat, double lon) {
  std::lock_guard<std::mutex> lock(position_mutex_);
  current_coordinate_ = {lat, lon};

  std::cout << "[ROVER] Position updated: Lat=" << lat << ", Lon=" << lon
            << std::endl;
}

bool Rover::is_low_power_mode() {
  std::lock_guard<std::mutex> lock(low_power_mutex_);
  return low_power_mode_;
}

// helper
std::vector<uint8_t> string_to_binary(const std::string &str) {
  return std::vector<uint8_t>(str.begin(), str.end());
}

// store messages if disconnected from base, for sending later
void Rover::store_message_for_later(const Message &message,
                                    const udp::endpoint &intended_recipient) {
  try {
    std::string json_str = message.serialise();
    std::vector<uint8_t> payload = string_to_binary(json_str);
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

// --- callback setters ---
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
    target_is_base = true; // telemetry always goes to base
  } catch (const std::runtime_error &e) {
    std::cerr << "[ROVER] Error getting base endpoint for telemetry: "
              << e.what() << std::endl;
    return; // cannot determine target
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  TelemetryMessage telemetry_msg(readings, rover_id_);

  bool lp_mode = is_low_power_mode();
  if (lp_mode &&
      telemetry_msg.get_lumen_priority() != LumenHeader::Priority::HIGH) {
    std::cout << "[ROVER] Suppressing telemetry message (Low Power Mode)."
              << std::endl;
    return; // don't send or store if low power and not high priority
  }

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
    // logic for sending telemetry to non-base (if ever needed)
    std::cerr
        << "[ROVER] Warning: Telemetry sending to non-base not implemented."
        << std::endl;
  } else {
    std::cout << "[ROVER] Cannot send telemetry, session not active or "
                 "disconnected targetting non-base."
              << std::endl;
  }
}

// main method for sending application messages via the protocol stack
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

  // determine final target and if it's the base station
  try {
    base_ep_check = get_base_endpoint();
    if (target_endpoint.address().is_unspecified() ||
        target_endpoint == base_ep_check) {
      target_endpoint = base_ep_check; // default or explicit base target
      target_is_base = true;
    }
  } catch (const std::runtime_error &e) {
    std::cerr << "[ROVER] Error checking base endpoint for send_message: "
              << e.what() << std::endl;
    // if base isn't registered, only proceed if recipient was explicit
    if (target_endpoint.address().is_unspecified()) {
      std::cout << "[ROVER] Cannot send message type " << message.get_type()
                << ": Base endpoint unknown and no specific recipient."
                << std::endl;
      return;
    }
    target_is_base = false; // assume not base if we can't resolve it
  }

  bool lp_mode = is_low_power_mode();
  if (lp_mode && message.get_lumen_priority() != LumenHeader::Priority::HIGH) {
    std::cout << "[ROVER] Suppressing message type " << message.get_type()
              << " (Low Power Mode)." << std::endl;
    return; // don't send or store if low power and not high priority
  }

  if (current_state == SessionState::DISCONNECTED && target_is_base) {
    store_message_for_later(message, target_endpoint);
  } else if (current_state == SessionState::ACTIVE || !target_is_base) {
    // send if active or if sending to non-base endpoint (allowed even if
    // disconnected from base)
    if (lp_mode && !target_is_base) {
      std::cout << "[ROVER] Suppressing rover-rover message type "
                << message.get_type() << " (Low Power Mode)." << std::endl;
      return;
    }
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

// sends message bypassing lumen protocol (raw json over udp)
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
  bool lp_mode = is_low_power_mode();
  if (lp_mode && message.get_lumen_priority() != LumenHeader::Priority::HIGH) {
    std::cout << "[ROVER] Suppressing raw message type " << message.get_type()
              << " (Low Power Mode)." << std::endl;
    return; // don't send if low power and not high priority
  }

  bool target_is_base = false;
  try {
    target_is_base = (recipient == get_base_endpoint());
  } catch (...) { /* ignore if base endpoint not resolved */
  }

  if (lp_mode && !target_is_base) {
    std::cout << "[ROVER] Suppressing raw rover-rover message type "
              << message.get_type() << " (Low Power Mode)." << std::endl;
    return;
  }
  // delegate raw sending to messagemanager
  message_manager_->send_raw_message(message, recipient);
  std::cout << "[ROVER] Sent raw (JSON) message type: " << message.get_type()
            << " to " << endpoint_to_string(recipient) << std::endl;
}

// --- status methods ---
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
    target_is_base = true; // status always goes to base
  } catch (const std::runtime_error &e) {
    std::cerr << "[ROVER] Error getting base endpoint for status: " << e.what()
              << std::endl;
    return; // cannot determine target
  }

  StatusMessage status_msg(current_level, current_desc, rover_id_);
  bool lp_mode = is_low_power_mode();
  if (lp_mode &&
      status_msg.get_lumen_priority() != LumenHeader::Priority::HIGH) {
    std::cout << "[ROVER] Suppressing status message (Low Power Mode)."
              << std::endl;
    return; // don't send or store if low power and not high priority
  }

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

// --- discovery methods ---
void Rover::scan_for_rovers(int discovery_port, const std::string &message) {
  if (is_low_power_mode()) {
    std::cout << "[ROVER] Suppressing rover scan (Low Power Mode)."
              << std::endl;
    return;
  }
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

    for (int i = 2; i <= 150; ++i) { // todo: make range configurable
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
      client_->send_data_to(data_to_send,
                            target_endpoint); // send to specific endpoint
    }

    std::cout << "[ROVER] Finished sending unicast scan packets." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[ROVER] Error during unicast scan: " << e.what() << std::endl;
  }
}

std::map<std::string, udp::endpoint> Rover::get_discovered_rovers() const {
  std::lock_guard<std::mutex> lock(discovery_mutex_);
  return discovered_rovers_; // return copy
}

void Rover::handle_discovery_command(CommandMessage *cmd_msg,
                                     const udp::endpoint &sender) {
  if (!cmd_msg)
    return;

  const std::string &command = cmd_msg->get_command();
  const std::string &sender_id =
      cmd_msg->get_sender(); // id of rover sending command

  // ignore messages from self
  if (sender_id == rover_id_) {
    return;
  }

  std::cout << "[ROVER DISCOVERY] Handling discovery command '" << command
            << "' from " << sender_id << " at " << endpoint_to_string(sender)
            << std::endl;

  // handle announcement from another rover
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

      // notify application layer via callback
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
  // handle receiving discovery probe from another rover
  else if (command == "ROVER_DISCOVER") {
    if (!is_low_power_mode()) {
      std::cout << "[ROVER DISCOVERY] Sending announcement." << std::endl;
      CommandMessage announce_msg("ROVER_ANNOUNCE", "Responding to probe",
                                  rover_id_);
      // send raw message (already checks low power mode for rover-rover)
      send_raw_message(announce_msg, sender);
    } else {
      std::cout << "[ROVER DISCOVERY] Suppressing announcement response (Low "
                   "Power Mode)."
                << std::endl;
    }
  }
}

// --- routing and internal command handling ---
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
    return; // stop processing self-sent messages
  }

  bool from_base_station = false;
  try {
    // check if sender matches registered base endpoint
    from_base_station = (client_ && sender == client_->get_base_endpoint());
  } catch (const std::runtime_error &e) {
    std::cerr
        << "[ROVER] Warning: Could not get base endpoint in route_message: "
        << e.what() << std::endl;
    // proceed assuming not from base if endpoint not registered/available
  }

  std::cout << "[ROVER INTERNAL] Routing message type: '" << msg_type
            << "' from sender ID: '" << sender_id << "' at "
            << endpoint_to_string(sender)
            << (from_base_station ? " (Base Station)" : " (Other)")
            << std::endl;

  // --- internal handling ---
  if (msg_type == CommandMessage::message_type()) {
    auto *cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    if (cmd_msg) {
      const std::string &command = cmd_msg->get_command();

      // session commands (must be from base station)
      if (command == "SESSION_ACCEPT" || command == "SESSION_ESTABLISHED" ||
          command == "SET_LOW_POWER" || command == "SET_TARGET_COORD") {
        if (from_base_station) {
          handle_internal_command(cmd_msg, sender);
        } else {
          std::cerr << "[ROVER] Warning: Ignoring session command '" << command
                    << "' from non-base endpoint: "
                    << endpoint_to_string(sender) << std::endl;
        }
        return; // session commands fully handled
      }
      // discovery commands (can be from any rover)
      else if (command == "ROVER_ANNOUNCE" || command == "ROVER_DISCOVER") {
        if (sender_id != rover_id_) {
          if (!is_low_power_mode()) { // only handle discovery if not in low
                                      // power
            handle_discovery_command(cmd_msg, sender);
          } else {
            std::cout << "[ROVER INTERNAL] Ignoring discovery message (Low "
                         "Power Mode)."
                      << std::endl;
          }
        } else {
          std::cout << "[ROVER INTERNAL] Ignoring self-discovery message."
                    << std::endl;
        }
        return; // discovery commands fully handled
      }
      // other potential internal commands could go here
    }
  }

  // --- application handling ---
  // if not handled internally, pass to application handler
  ApplicationMessageHandler handler_copy;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler_copy = application_message_handler_;
  }

  if (handler_copy) {
    // post to io_context to avoid blocking network thread
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
    // log if no handler exists for non-internal messages
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
  const std::string &params = cmd_msg->get_params();

  if (command == "SESSION_ACCEPT") {
    handle_session_accept();
  } else if (command == "SESSION_ESTABLISHED") {
    handle_session_established();
  } else if (command == "SET_LOW_POWER") {
    std::cout << "Received command:"
              << Message::pretty_print(cmd_msg->serialise()) << std::endl;
    bool enable = (params == "1");
    { // lock scope
      std::lock_guard<std::mutex> lock(low_power_mutex_);
      if (low_power_mode_ != enable) {
        low_power_mode_ = enable;
        std::cout << "[ROVER INTERNAL] Low Power Mode "
                  << (enable ? "ENABLED" : "DISABLED") << std::endl;
      } else {
        std::cout << "Already " << (enable ? "enabled." : "disabled.")
                  << std::endl;
      }
    }
  } else if (command == "SET_TARGET_COORD") {
    std::cout << "Received command:"
              << Message::pretty_print(cmd_msg->serialise()) << std::endl;
    std::cout << "[ROVER INTERNAL] Received SET_TARGET_COORD with params: "
              << params << std::endl;
    std::stringstream ss(params);
    std::string segment;
    std::vector<double> coords;

    while (std::getline(ss, segment, ',')) {
      try {
        coords.push_back(std::stod(segment));
      } catch (const std::exception &e) {
        std::cerr << "[ROVER INTERNAL] Failed to parse coordinate segment '"
                  << segment << "': " << e.what() << std::endl;
        coords.clear(); // invalidate if any part fails
        break;
      }
    }

    if (coords.size() == 2) {
      double target_lat = coords[0];
      double target_lon = coords[1];
      { // lock scope
        std::lock_guard<std::mutex> lock_target(target_coord_mutex_);
        std::lock_guard<std::mutex> lock_pos(
            position_mutex_); // lock current pos too

        target_coordinate_ = {target_lat, target_lon};
        std::cout << "[ROVER INTERNAL] Target coordinate set to: Lat="
                  << target_lat << ", Lon=" << target_lon << std::endl;

        // start movement timer only if not already at target
        double dist = calculate_distance(current_coordinate_.first,
                                         current_coordinate_.second, target_lat,
                                         target_lon);
        if (dist > TARGET_REACHED_THRESHOLD_METERS) {
          boost::system::error_code ec;
          movement_timer_.cancel(ec); // cancel any previous movement
          movement_timer_.expires_after(
              std::chrono::milliseconds(10)); // start quickly
          movement_timer_.async_wait(
              [this](const boost::system::error_code &ec) {
                if (!ec) {
                  handle_movement_timer();
                }
              });
          std::cout << "[ROVER INTERNAL] Movement timer started." << std::endl;
        } else {
          std::cout << "[ROVER INTERNAL] Already at target coordinate. "
                       "Movement timer not started."
                    << std::endl;
          target_coordinate_ = std::nullopt; // clear target if already there
        }
      }
    } else {
      std::cerr
          << "[ROVER INTERNAL] Invalid format for SET_TARGET_COORD params: "
          << params << ". Expected 'latitude,longitude'." << std::endl;
    }
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
    // if disconnected, cancel probe timer before starting handshake
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

  send_command("SESSION_INIT", rover_id_); // send init via normal protocol path
  std::cout << "[ROVER INTERNAL] Sent SESSION_INIT." << std::endl;

  // start handshake timer
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
  SessionState current_state = SessionState::INACTIVE;
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
      } else {
        send_command("SESSION_CONFIRM", rover_id_);
      } // handshake_accept

      // reschedule timer
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
    stored_packets_.clear(); // clear queue as we cannot send them
    return;
  }

  // send all stored packets using structured bindings
  while (!stored_packets_.empty()) {
    auto [payload, type, priority, recipient] =
        stored_packets_.front(); // deconstruct tuple

    try {
      protocol_->send_message(payload, type, priority, recipient);
      std::cout << "[ROVER INTERNAL] Sent stored packet type "
                << static_cast<int>(static_cast<uint8_t>(type)) << " to "
                << recipient << std::endl;
    } catch (const std::exception &e) {
      std::cerr << "[ROVER INTERNAL] Error sending stored packet: " << e.what()
                << std::endl;
    }
    stored_packets_.pop_front(); // remove sent/failed packet from queue
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
  Coordinate current_pos;
  double lat, lon;
  SessionState current_state;

  {
    std::lock_guard<std::mutex> lock(position_mutex_);
    current_pos = current_coordinate_;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  // attempt send telemetry; if disconnected, send_telemetry queues it
  std::map<std::string, double> position_data = {
      {"latitude", current_pos.first}, {"longitude", current_pos.second}};
  send_telemetry(position_data);

  // always reschedule timer as long as rover running
  boost::system::error_code ec;
  position_telemetry_timer_.expires_after(POSITION_TELEMETRY_INTERVAL);
  position_telemetry_timer_.async_wait(
      [this](const boost::system::error_code &ec) {
        if (!ec) {
          handle_position_telemetry_timer();
        } // reschedule regardless of state
        else if (ec != boost::asio::error::operation_aborted) {
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

  // attempt send status; if disconnected, send_status queues it
  send_status();

  // always reschedule timer as long as rover running
  boost::system::error_code ec;
  status_timer_.expires_after(STATUS_INTERVAL);
  status_timer_.async_wait([this](const boost::system::error_code &ec) {
    if (!ec) {
      handle_status_timer();
    } // reschedule regardless of state
    else if (ec != boost::asio::error::operation_aborted) {
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

// --- getters ---
Rover::SessionState Rover::get_session_state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return session_state_;
}

const udp::endpoint &Rover::get_base_endpoint() const {
  if (!client_) {
    throw std::runtime_error("[ROVER] Error: UdpClient is not initialized.");
  }
  // get_base_endpoint() in udpclient already throws if not registered
  return client_->get_base_endpoint();
}

// internal method to send commands, mainly for session mgmt
void Rover::send_command(const std::string &command,
                         const std::string &params) {
  if (!message_manager_) {
    std::cerr << "[ROVER] Error: Cannot send command '" << command
              << "', MessageManager is null." << std::endl;
    return;
  }
  CommandMessage cmd_msg(command, params, rover_id_);
  bool lp_mode = is_low_power_mode();
  if (lp_mode && cmd_msg.get_lumen_priority() != LumenHeader::Priority::HIGH) {
    std::cout << "[ROVER] Suppressing command '" << command
              << "' (Low Power Mode)." << std::endl;
    return; // don't send or store if low power and not high priority
  }
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
    // allow sending probe even when disconnected
    message_manager_->send_message(cmd_msg, base_ep);
    std::cout << "[ROVER] Sent SESSION_INIT probe while disconnected."
              << std::endl;
  } else if (current_state != SessionState::DISCONNECTED) {
    // send normally if not disconnected
    message_manager_->send_message(cmd_msg, base_ep);
  } else {
    // do not send other commands while disconnected
    std::cout << "[ROVER] Suppressed sending command '" << command
              << "' while disconnected." << std::endl;
  }
}

// handles transition to disconnected state from base
void Rover::handle_base_disconnect() {
  std::lock_guard<std::mutex> lock(state_mutex_);

  // check if already disconnected or inactive
  if (session_state_ != SessionState::ACTIVE) {
    std::cout << "[ROVER INTERNAL] Ignoring disconnect signal, not in ACTIVE "
                 "state (current state: "
              << static_cast<int>(session_state_) << ")." << std::endl;
    return;
  }

  std::cout << "[ROVER INTERNAL] Detected base station disconnect. "
               "Transitioning to DISCONNECTED state."
            << std::endl;
  session_state_ = SessionState::DISCONNECTED;

  // cancel timers that depend on active state
  boost::system::error_code ec;
  handshake_timer_.cancel(ec);
  probe_timer_.cancel(ec); // cancel existing probe timer first

  // reset protocol sequence number and reliability state
  boost::asio::post(io_context_, [this]() {
    if (protocol_) {
      protocol_->reset_sequence_number();
    } else {
      std::cerr << "[ROVER INTERNAL] Warning: Cannot reset sequence number, "
                   "protocol_ is null."
                << std::endl;
    }
  });

  // start probe timer to periodically try reconnecting
  boost::asio::post(io_context_, [this]() {
    probe_timer_.expires_after(std::chrono::seconds(0)); // start immediately
    probe_timer_.async_wait([this](const boost::system::error_code &error) {
      if (!error) {
        handle_probe_timer();
      } // schedule next probe check
      else if (error != boost::asio::error::operation_aborted) {
        std::cerr << "[ROVER INTERNAL] Probe timer wait error: "
                  << error.message() << std::endl;
      }
    });
    std::cout << "[ROVER INTERNAL] Started periodic probing timer."
              << std::endl;
  });
}

// periodic timer handler when disconnected, sends probe
void Rover::handle_probe_timer() {
  SessionState current_state;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state = session_state_;
  }

  // only probe if still disconnected
  if (current_state == SessionState::DISCONNECTED) {
    std::cout
        << "[ROVER INTERNAL] Sending probe (SESSION_INIT) to base station..."
        << std::endl;
    send_command("SESSION_INIT", rover_id_); // send probe

    // reschedule timer
    probe_timer_.expires_at(probe_timer_.expiry() + PROBE_INTERVAL);
    probe_timer_.async_wait([this](const boost::system::error_code &error) {
      if (!error) {
        handle_probe_timer();
      } // continue probing
      else if (error != boost::asio::error::operation_aborted) {
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
    // do not reschedule if state changed
  }
}

// called by reliability manager on packet timeout notification
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

  // only trigger disconnect logic if we were active and timeout was for base
  // station
  if (current_state == SessionState::ACTIVE && is_base_target) {
    std::cout
        << "[ROVER INTERNAL] Received timeout notification for base station."
        << std::endl;
    handle_base_disconnect(); // let reliabilitymanager trigger this via
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

// handles receiving session_accept from base
void Rover::handle_session_accept() {
  std::cout << "[ROVER INTERNAL] Received SESSION_ACCEPT." << std::endl;
  bool state_updated = false;
  SessionState previous_state;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    previous_state = session_state_;

    // allow transition from handshake_init or disconnected (if probe received
    // accept)
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
  } // lock released

  if (state_updated) {
    // cancel timers associated with previous states
    boost::system::error_code ec_handshake, ec_probe;
    handshake_timer_.cancel(ec_handshake); // cancel handshake retry timer

    // crucially, cancel probe timer if we were disconnected
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

    // proceed to confirm session
    send_command("SESSION_CONFIRM", rover_id_);
    std::cout << "[ROVER INTERNAL] Sent SESSION_CONFIRM." << std::endl;

    // re-arm handshake timer to wait for session_established
    boost::asio::post(io_context_, [this]() {
      handshake_timer_.expires_after(HANDSHAKE_TIMEOUT);
      handshake_timer_.async_wait([this](const boost::system::error_code &ec) {
        if (!ec) {
          handle_handshake_timer();
        } // handle timeout waiting for established
        else if (ec != boost::asio::error::operation_aborted) {
          std::cerr
              << "[ROVER INTERNAL] Handshake timer (wait ESTABLISHED) error: "
              << ec.message() << std::endl;
        }
      });
    });
  }
}

// periodic timer handler for simulated movement towards target
void Rover::handle_movement_timer() {
  Coordinate target_pos;
  Coordinate current_pos;
  bool target_set = false;

  { // lock scope for target and current position
    std::lock_guard<std::mutex> lock_target(target_coord_mutex_);
    std::lock_guard<std::mutex> lock_pos(position_mutex_);

    if (!target_coordinate_) {
      std::cout
          << "[ROVER MOVEMENT] Timer fired but no target set. Stopping timer."
          << std::endl;
      return; // no target, stop
    }
    target_set = true;
    target_pos = *target_coordinate_;
    current_pos = current_coordinate_;
  } // locks released

  if (!target_set)
    return; // should not happen

  double current_x = current_pos.first;
  double current_y = current_pos.second;
  double target_x = target_pos.first;
  double target_y = target_pos.second;

  double distance_to_target =
      calculate_distance(current_x, current_y, target_x, target_y);

  std::cout << "[ROVER MOVEMENT] Current: (" << current_x << ", " << current_y
            << "), Target: (" << target_x << ", " << target_y
            << "), Dist: " << distance_to_target << "m" << std::endl;

  // check if target reached
  if (distance_to_target <= TARGET_REACHED_THRESHOLD_METERS) {
    std::cout << "[ROVER MOVEMENT] Target coordinates reached!" << std::endl;
    { // lock scope to clear target
      std::lock_guard<std::mutex> lock(target_coord_mutex_);
      target_coordinate_ = std::nullopt; // clear target
    }

    // send notification to base station
    BasicMessage reached_msg(
        "Rover " + rover_id_ + " reached target coordinates.", rover_id_);
    try {
      // ensure session active or store message if necessary
      send_message(reached_msg, get_base_endpoint());
    } catch (const std::exception &e) {
      std::cerr << "[ROVER MOVEMENT] Failed to send TARGET_REACHED message: "
                << e.what() << std::endl;
      // optionally store for later if disconnected
      // store_message_for_later(reached_msg, get_base_endpoint());
    }
    return; // stop timer (don't reschedule)
  }

  // calculate movement step distance based on rate and interval
  double time_step_sec =
      std::chrono::duration<double>(MOVEMENT_INTERVAL).count();
  double step_distance = MOVE_RATE_METERS_PER_SECOND * time_step_sec;

  // prevent overshooting target
  if (step_distance > distance_to_target) {
    step_distance = distance_to_target;
  }

  // calculate direction vector (normalized)
  double vector_x = target_x - current_x;
  double vector_y = target_y - current_y;
  double direction_x = vector_x / distance_to_target;
  double direction_y = vector_y / distance_to_target;

  // calculate next position
  double next_x = current_x + direction_x * step_distance;
  double next_y = current_y + direction_y * step_distance;

  // update position (thread-safe via existing function)
  update_current_position(next_x, next_y);

  // reschedule timer for next movement step
  boost::system::error_code ec;
  movement_timer_.expires_at(movement_timer_.expiry() + MOVEMENT_INTERVAL);
  movement_timer_.async_wait([this](const boost::system::error_code &ec) {
    if (!ec) {
      handle_movement_timer();
    } // call again if timer wasn't cancelled
    else if (ec != boost::asio::error::operation_aborted) {
      std::cerr << "[ROVER MOVEMENT] Timer error: " << ec.message()
                << std::endl;
    }
    // if operation_aborted, do nothing (timer cancelled)
  });
}