// src/common/lumen_protocol.cpp

#include "lumen_protocol.hpp"
#include "configs.hpp"
#include "lumen_header.hpp"
#include "lumen_packet.hpp"
#include "message_manager.hpp"
#include "reliability_manager.hpp"

#include <chrono>
#include <iostream>
#include <sstream>

// Constructor for base station
LumenProtocol::LumenProtocol(boost::asio::io_context &io_context,
                             UdpServer &server)
    : mode_(ProtocolMode::BASE_STATION), server_(&server), client_(nullptr),
      current_sequence_(0),
      reliability_manager_(std::make_unique<ReliabilityManager>(
          io_context, true)), // Base station
      running_(false), io_context_(io_context) {

  // Set up retransmission callback
  reliability_manager_->set_retransmit_callback(
      [this](const LumenPacket &packet, const udp::endpoint &endpoint) {
        send_packet(packet, endpoint);
      });

  // Set up callback to handle UDP data
  server_->set_receive_callback(
      [this](const std::vector<uint8_t> &data, const udp::endpoint &endpoint) {
        handle_udp_data(data, endpoint);
      });

  std::cout << "[LUMEN] Created protocol in BASE_STATION mode" << std::endl;
}
// Constructor for rover
LumenProtocol::LumenProtocol(boost::asio::io_context &io_context,
                             UdpClient &client)
    : mode_(ProtocolMode::ROVER), server_(nullptr), client_(&client),
      current_sequence_(0),
      reliability_manager_(
          std::make_unique<ReliabilityManager>(io_context, false)), // Rover
      running_(false), io_context_(io_context) {

  // Set up retransmission callback
  reliability_manager_->set_retransmit_callback(
      [this](const LumenPacket &packet, const udp::endpoint &endpoint) {
        send_packet(packet, endpoint);
      });

  // Set up callback to handle UDP data
  client_->set_receive_callback([this](const std::vector<uint8_t> &data) {
    handle_udp_data(data, client_->get_base_endpoint());
  });

  std::cout << "[LUMEN] Created protocol in ROVER mode" << std::endl;
}

LumenProtocol::~LumenProtocol() { stop(); }

void LumenProtocol::start() {
  if (running_)
    return;

  running_ = true;

  // clear frame buffer
  {
    std::lock_guard<std::mutex> lock(frame_buffers_mutex_);
    frame_buffers_.clear();
  }

  {
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    endpoint_map_.clear();
  }

  // start reliability manager
  reliability_manager_->start();

  std::cout << "[LUMEN] Protocol started in "
            << (mode_ == ProtocolMode::BASE_STATION ? "BASE_STATION" : "ROVER")
            << " mode." << std::endl;
}

void LumenProtocol::stop() {
  if (!running_)
    return;

  running_ = false;

  // stop reliability manager
  reliability_manager_->stop();

  std::cout << "[LUMEN] Protocol stopped." << std::endl;
}

void LumenProtocol::send_message(const std::vector<uint8_t> &payload,
                                 LumenHeader::MessageType type,
                                 LumenHeader::Priority priority,
                                 const udp::endpoint &recipient) {
  if (!running_) {
    std::cerr << "[ERROR] LumenProtocol not running" << std::endl;
    return;
  }

  // get next sequence number
  uint8_t seq = current_sequence_++;

  // generate timestamp
  uint32_t timestamp = generate_timestamp();

  // create header
  LumenHeader header(type, priority, seq, timestamp,
                     static_cast<uint16_t>(payload.size()));

  // create packet
  LumenPacket packet(header, payload);

  // send the packet
  udp::endpoint target_endpoint;

  if (mode_ == ProtocolMode::BASE_STATION) {
    // in server mode, we need a specific recipient
    if (recipient.address().is_unspecified()) {
      std::cerr << "[ERROR] No recipient specified for base station mode"
                << std::endl;
      return;
    }
    target_endpoint = recipient;
  } else {
    // in rover mode, send to base station
    target_endpoint = client_->get_base_endpoint();
  }

  // Only add the packet to reliability manager if it's not an ACK or NAK
  if (type != LumenHeader::MessageType::ACK &&
      type != LumenHeader::MessageType::NAK) {
    // track the packet in case we need to retransmit
    reliability_manager_->add_send_packet(seq, packet, target_endpoint);
  }

  // send the packet
  send_packet(packet, target_endpoint);

  std::cout << "[LUMEN] Sent packet with seq: " << static_cast<int>(seq)
            << ", type: " << static_cast<int>(static_cast<uint8_t>(type))
            << ", size: " << payload.size() << " to " << target_endpoint
            << std::endl;
}

std::string
LumenProtocol::get_endpoint_key(const udp::endpoint &endpoint) const {
  std::stringstream ss;
  ss << endpoint.address().to_string() << ":" << endpoint.port();
  return ss.str();
}

void LumenProtocol::set_message_callback(
    std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                       const udp::endpoint &)>
        callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  message_callback_ = std::move(callback);
}

uint8_t LumenProtocol::get_current_sequence() const {
  return current_sequence_.load();
}

void LumenProtocol::handle_udp_data(const std::vector<uint8_t> &data,
                                    const udp::endpoint &endpoint) {
  if (!running_)
    return; //

  // Check if this is potentially a raw JSON message (doesn't start with STX
  // marker)
  if (!data.empty() && data[0] != LUMEN_STX) { //
    // Check if it looks like JSON (starts with '{')
    if (data[0] == '{') { //
      std::cout << "[LUMEN] Detected raw JSON message without protocol headers"
                << std::endl; //

      // Convert binary data to string
      std::string json_str(data.begin(), data.end()); //

      // Attempt to deserialize the raw JSON
      try {
        if (Message::is_valid_json(json_str)) {          //
          auto message = Message::deserialise(json_str); //
          std::cout << "[MESSAGE MANAGER] Received raw JSON message: \n"
                    << Message::pretty_print(json_str)
                    << std::endl; // Pretty print

          // Get a copy of the callback set by MessageManager
          std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                             const udp::endpoint &)>
              lumen_msg_callback_copy;
          {                                                    //
            std::lock_guard<std::mutex> lock(callback_mutex_); //
            lumen_msg_callback_copy = message_callback_;       //
          }

          // ** FIX: Call the MessageManager's callback directly **
          // We bypass the normal LUMEN payload extraction, so call the
          // callback that expects the payload, header, and sender.
          if (lumen_msg_callback_copy) { //
            // Create a dummy header since there isn't one for raw JSON
            LumenHeader dummy_header(
                LumenHeader::MessageType::DATA, // Default type
                LumenHeader::Priority::MEDIUM,  // Default prio
                0,                              // Dummy sequence
                generate_timestamp(),           // Current timestamp
                0); // Zero payload length (header only)

            // Convert message back to binary payload temporarily for the
            // callback OR - Modify the callback system later to handle raw
            // Messages directly For now, convert the string back to
            // vector<uint8_t>
            std::vector<uint8_t> raw_payload(json_str.begin(), json_str.end());

            // Call the callback usually called by process_complete_packet
            lumen_msg_callback_copy(raw_payload, dummy_header, endpoint); //
          }
          // Successfully processed raw JSON, return to avoid LUMEN framing
          // logic
          return; //
        } else {
          std::cerr
              << "[LUMEN] Received data looks like JSON but failed validation."
              << std::endl;
        }
      } catch (const std::exception &e) { //
        std::cerr << "[ERROR] Failed to process raw JSON: " << e.what()
                  << std::endl; //
      }
      // If JSON parsing failed or it wasn't valid JSON, let it fall through
      // potentially to be discarded by LUMEN STX check later, or handle error
      // differently. For now, we let it fall through. Consider adding specific
      // error handling if needed.
    } else {
      // Data doesn't start with STX or '{'. Likely invalid/corrupt. Log or
      // ignore.
      std::cerr << "[LUMEN] Received UDP data without STX or starting '{'. "
                   "Discarding "
                << data.size() << " bytes from " << endpoint << std::endl;
      return; // Discard invalid data
    }
  }

  // ** Normal Lumen packet processing continues here if data started with STX
  // **
  std::string endpoint_key = get_endpoint_key(endpoint); //

  // Store the endpoint mapping
  {                                                    //
    std::lock_guard<std::mutex> lock(endpoint_mutex_); //
    endpoint_map_[endpoint_key] = endpoint;            //
  }

  // add data to frame buffer
  {                                                         //
    std::lock_guard<std::mutex> lock(frame_buffers_mutex_); //
    frame_buffers_[endpoint_key].insert(frame_buffers_[endpoint_key].end(),
                                        data.begin(), data.end()); //
  }

  // process frame buffer to extract complete packets
  process_frame_buffer(endpoint_key, endpoint); //
}

void LumenProtocol::process_frame_buffer(const std::string &endpoint_key,
                                         const udp::endpoint &endpoint) {
  std::lock_guard<std::mutex> lock(frame_buffers_mutex_);

  if (frame_buffers_.find(endpoint_key) == frame_buffers_.end()) {
    return;
  }

  auto &buffer = frame_buffers_[endpoint_key];

  while (!buffer.empty()) {
    // Try to parse a complete packet
    auto packet_opt = LumenPacket::from_bytes(buffer);
    if (!packet_opt) {
      // No complete packet available yet
      break;
    }

    LumenPacket packet = *packet_opt;
    uint16_t packet_size = packet.total_size();

    // Remove processed data from buffer
    if (packet_size <= buffer.size()) {
      buffer.erase(buffer.begin(), buffer.begin() + packet_size);
    } else {
      // Should never happen if we have a valid packet
      break;
    }

    // Process the complete packet
    process_complete_packet(packet, endpoint);
  }

  // Prevent buffer overflow by truncating if too large
  if (buffer.size() > MAX_FRAME_BUFFER_SIZE) {
    std::cerr << "[LUMEN] Frame buffer overflow for endpoint " << endpoint_key
              << ", clearing buffer" << std::endl;
    buffer.clear();
  }
}

void LumenProtocol::process_complete_packet(const LumenPacket &packet,
                                            const udp::endpoint &endpoint) {
  // Extract header and payload
  const LumenHeader &header = packet.get_header();
  const std::vector<uint8_t> &payload = packet.get_payload();

  // Get message type and sequence
  LumenHeader::MessageType type = header.get_type();
  uint8_t seq = header.get_sequence();

  std::cout << "[LUMEN] Received packet with seq: " << static_cast<int>(seq)
            << ", type: " << static_cast<int>(static_cast<uint8_t>(type))
            << ", size: " << payload.size() << " from " << endpoint
            << std::endl;

  // Record this sequence as received (important for ALL packet types)
  reliability_manager_->record_received_sequence(seq, endpoint);

  // Handle ACK packets (Rover expects to receive these)
  if (type == LumenHeader::MessageType::ACK) {
    if (mode_ == ProtocolMode::ROVER && payload.size() >= 1) {
      uint8_t acked_seq = payload[0];
      std::cout << "[LUMEN] Processing ACK for seq: "
                << static_cast<int>(acked_seq) << std::endl;
      reliability_manager_->process_ack(acked_seq);
    } else if (mode_ == ProtocolMode::BASE_STATION) {
      std::cout << "[LUMEN] Ignoring unexpected ACK in BASE_STATION mode"
                << std::endl;
    }
    return;
  }

  // Handle NAK packets (Base station expects to receive these)
  if (type == LumenHeader::MessageType::NAK) {
    if (mode_ == ProtocolMode::BASE_STATION && payload.size() >= 1) {
      uint8_t requested_seq = payload[0];
      std::cout << "[LUMEN] Processing NAK for seq: "
                << static_cast<int>(requested_seq) << std::endl;
      reliability_manager_->process_nak(requested_seq);
    } else if (mode_ == ProtocolMode::ROVER) {
      std::cout << "[LUMEN] Ignoring unexpected NAK in ROVER mode" << std::endl;
    }
    return;
  }

  // Base station sends ACKs for all non-control packets
  if (mode_ == ProtocolMode::BASE_STATION) {
    send_ack(seq, endpoint);
  }

  // Rover checks for missing packets periodically
  if (mode_ == ProtocolMode::ROVER) {
    // Only check every few received packets to avoid excessive NAK traffic
    static uint8_t check_counter = 0;
    if (++check_counter % 5 == 0) {
      check_sequence_gaps(endpoint);
    }
  }

  // Forward the packet to the message callback for application processing
  std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                     const udp::endpoint &)>
      callback_copy;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_copy = message_callback_;
  }

  if (callback_copy) {
    callback_copy(payload, header, endpoint);
  }
}

// Completely rewritten function to check for sequence gaps
void LumenProtocol::check_sequence_gaps(const udp::endpoint &endpoint) {
  // Get missing sequences within our window
  std::vector<uint8_t> missing_seqs =
      reliability_manager_->get_missing_sequences(endpoint);

  // Limit the number of NAKs per check to avoid flooding
  const int MAX_NAKS_PER_CHECK = 3;
  int nak_count = 0;

  for (uint8_t missing_seq : missing_seqs) {
    // Skip sequence 0 which might not exist
    if (missing_seq == 0)
      continue;

    // Don't send duplicate NAKs for sequences we've recently NAKed
    if (!reliability_manager_->is_recently_naked(missing_seq)) {
      send_nak(missing_seq, endpoint);
      reliability_manager_->record_nak_sent(missing_seq);

      if (++nak_count >= MAX_NAKS_PER_CHECK)
        break;
    }
  }
}

void LumenProtocol::send_packet(const LumenPacket &packet,
                                const udp::endpoint &recipient) {
  // serialize packet to bytes
  std::vector<uint8_t> data = packet.to_bytes();

  try {
    // send with appropriate mode
    if (mode_ == ProtocolMode::BASE_STATION) {
      server_->send_data(data, recipient);
    } else {
      // Properly handle the endpoint in rover mode
      if (recipient == client_->get_base_endpoint()) {
        client_->send_data(data); // Standard path to base station
      } else {
        client_->send_data_to(data, recipient); // Specific endpoint
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Failed to send packet: " << e.what() << std::endl;
  }
}

void LumenProtocol::send_ack(uint8_t seq, const udp::endpoint &recipient) {
  if (mode_ != ProtocolMode::BASE_STATION) {
    std::cerr << "[ERROR] ROVER should not send ACKs" << std::endl;
    return;
  }

  // Record this ACK to prevent retransmitting the packet later
  reliability_manager_->record_acked_sequence(seq, recipient);

  // Create ACK payload with sequence number
  std::vector<uint8_t> ack_payload = {seq};

  // Create ACK header with next sequence number
  uint32_t timestamp = generate_timestamp();
  uint8_t ack_seq = current_sequence_++;

  LumenHeader ack_header(LumenHeader::MessageType::ACK,
                         LumenHeader::Priority::HIGH, ack_seq, timestamp,
                         static_cast<uint16_t>(ack_payload.size()));

  // Create and send ACK packet
  LumenPacket ack_packet(ack_header, ack_payload);
  send_packet(ack_packet, recipient);

  std::cout << "[LUMEN] Sent ACK for seq: " << static_cast<int>(seq)
            << " in packet with seq: " << static_cast<int>(ack_seq) << " to "
            << recipient << std::endl;
}

// Send a NAK packet (Rover only)
void LumenProtocol::send_nak(uint8_t seq, const udp::endpoint &recipient) {
  if (mode_ != ProtocolMode::ROVER) {
    std::cerr << "[ERROR] BASE_STATION should not send NAKs" << std::endl;
    return;
  }

  // Create NAK payload with sequence number
  std::vector<uint8_t> nak_payload = {seq};

  // Create NAK header
  uint32_t timestamp = generate_timestamp();
  uint8_t nak_seq = current_sequence_++;

  LumenHeader nak_header(LumenHeader::MessageType::NAK,
                         LumenHeader::Priority::HIGH, nak_seq, timestamp,
                         static_cast<uint16_t>(nak_payload.size()));

  // Create and send NAK packet
  LumenPacket nak_packet(nak_header, nak_payload);
  send_packet(nak_packet, recipient);

  std::cout << "[LUMEN] Sent NAK for seq: " << static_cast<int>(seq) << " to "
            << recipient << std::endl;
}

uint32_t LumenProtocol::generate_timestamp() const {
  // get current time as milliseconds since epoch
  auto now = std::chrono::system_clock::now();
  auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch())
                    .count();

  // return as 32-bit timestamp
  return static_cast<uint32_t>(millis & 0xFFFFFFFF);
}

void LumenProtocol::handle_retransmission(const LumenPacket &packet,
                                          const udp::endpoint &endpoint) {
  std::cout << "[LUMEN] Retransmitting packet with seq: "
            << static_cast<int>(packet.get_header().get_sequence()) << " to "
            << endpoint << std::endl;

  // resend the packet
  send_packet(packet, endpoint);
}