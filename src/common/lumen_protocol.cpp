// src/common/lumen_protocol.cpp

#include "lumen_protocol.hpp"
#include "configs.hpp"
#include "lumen_header.hpp"
#include "lumen_packet.hpp"
#include "message_manager.hpp" // Included for Message::deserialise, Message::is_valid_json
#include "reliability_manager.hpp"

#include <chrono>
#include <iostream>
#include <sstream>

LumenProtocol::LumenProtocol(boost::asio::io_context &io_context,
                             UdpServer &server)
    : mode_(ProtocolMode::BASE_STATION), server_(&server), client_(nullptr),
      current_sequence_(0),
      reliability_manager_(std::make_unique<ReliabilityManager>(
          io_context,
          true)), // Instantiate ReliabilityManager for Base Station mode
      running_(false), io_context_(io_context) {

  // Provide the ReliabilityManager with a callback to retransmit packets via
  // this LumenProtocol instance
  reliability_manager_->set_retransmit_callback(
      [this](const LumenPacket &packet, const udp::endpoint &endpoint) {
        send_packet(packet, endpoint);
      });

  // Set the callback for the underlying UdpServer to pass received data up to
  // this protocol layer
  server_->set_receive_callback(
      [this](const std::vector<uint8_t> &data, const udp::endpoint &endpoint) {
        handle_udp_data(data, endpoint);
      });

  std::cout << "[LUMEN] Created protocol in BASE_STATION mode" << std::endl;
}

LumenProtocol::LumenProtocol(boost::asio::io_context &io_context,
                             UdpClient &client)
    : mode_(ProtocolMode::ROVER), server_(nullptr), client_(&client),
      current_sequence_(0),
      reliability_manager_(std::make_unique<ReliabilityManager>(
          io_context, false)), // Instantiate ReliabilityManager for Rover mode
      running_(false), io_context_(io_context) {

  // Provide the ReliabilityManager with a callback to retransmit packets via
  // this LumenProtocol instance
  reliability_manager_->set_retransmit_callback(
      [this](const LumenPacket &packet, const udp::endpoint &endpoint) {
        send_packet(packet, endpoint);
      });

  // Set the callback for the underlying UdpClient to pass received data up to
  // this protocol layer Note: The client receives data and assumes it's from
  // the base station endpoint it's registered with.
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

  // Clear any stale data from previous runs
  {
    std::lock_guard<std::mutex> lock(frame_buffers_mutex_);
    frame_buffers_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    endpoint_map_.clear();
  }

  // Start the reliability manager's timers and logic
  reliability_manager_->start();

  std::cout << "[LUMEN] Protocol started in "
            << (mode_ == ProtocolMode::BASE_STATION ? "BASE_STATION" : "ROVER")
            << " mode." << std::endl;
}

void LumenProtocol::stop() {
  if (!running_)
    return;

  running_ = false;

  // Stop the reliability manager's timers
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

  uint8_t seq =
      current_sequence_++; // Get next sequence number (atomic increment)
  uint32_t timestamp = generate_timestamp();
  LumenHeader header(type, priority, seq, timestamp,
                     static_cast<uint16_t>(payload.size()));
  LumenPacket packet(header, payload);

  udp::endpoint target_endpoint;
  if (mode_ == ProtocolMode::BASE_STATION) {
    // Base station needs an explicit recipient for each message
    if (recipient.address().is_unspecified()) {
      std::cerr << "[ERROR] No recipient specified for base station mode"
                << std::endl;
      return;
    }
    target_endpoint = recipient;
  } else {
    // Rover sends messages to its registered base station by default
    target_endpoint = client_->get_base_endpoint();
  }

  // ReliabilityManager only needs to track packets that require acknowledgment
  // or potential retransmission. ACKs and NAKs themselves are control packets
  // not tracked for reliability.
  if (type != LumenHeader::MessageType::ACK &&
      type != LumenHeader::MessageType::NAK) {
    reliability_manager_->add_send_packet(seq, packet, target_endpoint);
  }

  send_packet(packet, target_endpoint);

  std::cout << "[LUMEN] Sent packet with seq: " << static_cast<int>(seq)
            << ", type: " << static_cast<int>(static_cast<uint8_t>(type))
            << ", size: " << payload.size() << " to " << target_endpoint
            << std::endl;
}

// Helper to create a consistent string key from an endpoint for map lookups.
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
    return;

  // --- Handling for Raw JSON (without LUMEN headers) ---
  // This allows simpler communication for testing or specific cases where the
  // full protocol overhead isn't needed.
  if (!data.empty() && data[0] != LUMEN_STX) {
    // Check if it looks like a JSON object
    if (data[0] == '{') {
      std::cout << "[LUMEN] Detected potential raw JSON message without "
                   "protocol headers from "
                << endpoint << std::endl;
      std::string json_str(data.begin(), data.end());

      try {
        // Attempt to deserialize directly using the Message factory
        if (Message::is_valid_json(json_str)) { // Check validity first
          auto message = Message::deserialise(json_str);
          std::cout << "[LUMEN] Successfully deserialized raw JSON message."
                    << std::endl;

          // Get the callback registered by MessageManager
          std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                             const udp::endpoint &)>
              lumen_msg_callback_copy;
          {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            lumen_msg_callback_copy = message_callback_;
          }

          // Since this bypasses normal LUMEN processing, directly invoke the
          // callback expected by MessageManager. A dummy header is created as
          // the callback signature requires it.
          if (lumen_msg_callback_copy) {
            LumenHeader dummy_header(
                LumenHeader::MessageType::DATA, // Assume DATA type
                LumenHeader::Priority::MEDIUM,  // Assume MEDIUM priority
                0,                              // Sequence not applicable
                generate_timestamp(),
                static_cast<uint16_t>(
                    data.size())); // Use raw data size for length

            // Pass the original raw binary data as payload
            lumen_msg_callback_copy(data, dummy_header, endpoint);
          }
          // Successfully processed as raw JSON, exit the handler.
          return;
        } else {
          std::cerr << "[LUMEN] Received data from " << endpoint
                    << " starting with '{' but failed JSON validation."
                    << std::endl;
        }
      } catch (const std::exception &e) {
        std::cerr << "[ERROR] Failed to process raw JSON from " << endpoint
                  << ": " << e.what() << std::endl;
      }
      // If JSON processing failed, let it fall through to be discarded below.
    } else {
      // Data doesn't start with STX or '{'. Discard as invalid/unrecognized.
      std::cerr << "[LUMEN] Received UDP data from " << endpoint
                << " without STX or starting '{'. Discarding " << data.size()
                << " bytes." << std::endl;
      return;
    }
  }
  // --- End Raw JSON Handling ---

  // --- Normal LUMEN Packet Processing ---
  std::string endpoint_key = get_endpoint_key(endpoint);

  // Store the actual endpoint object associated with this key, useful if
  // multiple endpoints send data.
  {
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    endpoint_map_[endpoint_key] = endpoint;
  }

  // Append received data to the per-endpoint buffer for potential reassembly
  {
    std::lock_guard<std::mutex> lock(frame_buffers_mutex_);
    frame_buffers_[endpoint_key].insert(frame_buffers_[endpoint_key].end(),
                                        data.begin(), data.end());
  }

  // Attempt to process complete LUMEN packets from the buffer
  process_frame_buffer(endpoint_key, endpoint);
}

void LumenProtocol::process_frame_buffer(const std::string &endpoint_key,
                                         const udp::endpoint &endpoint) {
  std::lock_guard<std::mutex> lock(frame_buffers_mutex_);

  // Ensure the buffer for this endpoint exists
  if (frame_buffers_.find(endpoint_key) == frame_buffers_.end()) {
    return;
  }

  auto &buffer = frame_buffers_[endpoint_key];

  // Process as many complete packets as possible from the start of the buffer
  while (!buffer.empty()) {
    // LumenPacket::from_bytes checks for STX, valid header, length, CRC, and
    // ETX.
    auto packet_opt = LumenPacket::from_bytes(buffer);
    if (!packet_opt) {
      // Not enough data for a complete packet, or data is malformed at the
      // start. If malformed, need a mechanism to find the next STX, otherwise
      // wait for more data. For now, we assume well-formed streams or wait. A
      // robust implementation might scan for STX.
      break;
    }

    LumenPacket packet = *packet_opt;
    size_t packet_size =
        packet.total_size(); // Get size *including* headers, CRC, ETX

    // Remove the processed packet bytes from the buffer
    if (packet_size <= buffer.size()) {
      buffer.erase(buffer.begin(), buffer.begin() + packet_size);
    } else {
      std::cerr << "[LUMEN] Internal error: Packet size (" << packet_size
                << ") > buffer size (" << buffer.size()
                << ") after successful parse. Buffer state might be corrupted."
                << std::endl;
      buffer.clear(); // Clear buffer to prevent potential infinite loops
      break;          // Should not happen if from_bytes is correct
    }

    // Pass the validated, complete packet for processing
    // Release the lock before calling process_complete_packet to avoid
    // potential deadlocks if it calls back into LumenProtocol No, keep lock:
    // process_complete_packet accesses ReliabilityManager which is thread-safe,
    // but simpler to keep lock here.
    process_complete_packet(packet, endpoint);
  }

  // Basic protection against buffer growing indefinitely if data is corrupted
  // or packets are huge.
  if (buffer.size() > MAX_FRAME_BUFFER_SIZE) {
    std::cerr << "[LUMEN] Frame buffer for endpoint " << endpoint_key
              << " exceeded max size (" << buffer.size() << " > "
              << MAX_FRAME_BUFFER_SIZE << "). Clearing buffer." << std::endl;
    buffer.clear();
  }
}

void LumenProtocol::process_complete_packet(const LumenPacket &packet,
                                            const udp::endpoint &endpoint) {
  const LumenHeader &header = packet.get_header();
  const std::vector<uint8_t> &payload = packet.get_payload();
  LumenHeader::MessageType type = header.get_type();
  uint8_t seq = header.get_sequence();

  std::cout << "[LUMEN] Received packet seq: " << static_cast<int>(seq)
            << ", type: " << static_cast<int>(static_cast<uint8_t>(type))
            << ", size: " << payload.size() << " from " << endpoint
            << std::endl;

  // Track every received sequence number for gap detection (in Rover mode)
  reliability_manager_->record_received_sequence(seq, endpoint);

  // --- Handle Control Packets (ACK/NAK) ---
  if (type == LumenHeader::MessageType::ACK) {
    // Rover expects ACKs from the Base Station
    if (mode_ == ProtocolMode::ROVER && payload.size() >= 1) {
      uint8_t acked_seq = payload[0]; // ACK payload contains the sequence
                                      // number being acknowledged
      std::cout << "[LUMEN] Processing ACK for original seq: "
                << static_cast<int>(acked_seq) << std::endl;
      reliability_manager_->process_ack(acked_seq);
    } else if (mode_ == ProtocolMode::BASE_STATION) {
      std::cout << "[LUMEN] Warning: Ignoring unexpected ACK received in "
                   "BASE_STATION mode from "
                << endpoint << std::endl;
    }
    return; // ACKs are processed here, no further callback needed
  }

  if (type == LumenHeader::MessageType::NAK) {
    // Base Station expects NAKs from the Rover
    if (mode_ == ProtocolMode::BASE_STATION && payload.size() >= 1) {
      uint8_t requested_seq = payload[0]; // NAK payload contains the sequence
                                          // number being requested
      std::cout << "[LUMEN] Processing NAK for missing seq: "
                << static_cast<int>(requested_seq) << std::endl;
      reliability_manager_->process_nak(
          requested_seq); // ReliabilityManager handles retransmission
    } else if (mode_ == ProtocolMode::ROVER) {
      std::cout << "[LUMEN] Warning: Ignoring unexpected NAK received in ROVER "
                   "mode from "
                << endpoint << std::endl;
    }
    return; // NAKs are processed here, no further callback needed
  }
  // --- End Control Packet Handling ---

  // --- Reliability Actions for Data/Status/Cmd packets ---
  // Base station sends an ACK for every non-control packet it receives
  // successfully.
  if (mode_ == ProtocolMode::BASE_STATION) {
    // Check if we already ACKed this sequence to prevent sending duplicate ACKs
    // if the sender retransmits.
    if (!reliability_manager_->has_acked_sequence(seq, endpoint)) {
      send_ack(seq, endpoint);
    } else {
      std::cout << "[LUMEN] Ignoring duplicate packet seq: "
                << static_cast<int>(seq) << " from " << endpoint
                << " (already ACKed)." << std::endl;
      return; // Do not process duplicate packet further
    }
  }

  // Rover checks for gaps periodically after receiving packets.
  if (mode_ == ProtocolMode::ROVER) {
    // Check occasionally to avoid sending NAKs for every single packet.
    static uint8_t check_counter = 0;
    if (++check_counter % 5 == 0) { // Check roughly every 5 packets
      check_sequence_gaps(endpoint);
    }
  }
  // --- End Reliability Actions ---

  // --- Forward Payload to Upper Layer (MessageManager) ---
  // Get a thread-safe copy of the callback function pointer.
  std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                     const udp::endpoint &)>
      callback_copy;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_copy = message_callback_;
  }

  if (callback_copy) {
    // Pass the extracted payload, header (containing metadata), and sender
    // endpoint to the MessageManager
    callback_copy(payload, header, endpoint);
  } else {
    std::cerr << "[LUMEN] Warning: No message callback set. Discarding payload "
                 "for packet seq: "
              << static_cast<int>(seq) << std::endl;
  }
}

// Checks for missing sequence numbers (Rover only).
void LumenProtocol::check_sequence_gaps(const udp::endpoint &endpoint) {
  // Ask ReliabilityManager for sequences deemed missing based on its tracked
  // received sequences.
  std::vector<uint8_t> missing_seqs =
      reliability_manager_->get_missing_sequences(endpoint);

  // Limit the number of NAKs sent at once to prevent flooding the network.
  const int MAX_NAKS_PER_CHECK = 3;
  int nak_count = 0;

  for (uint8_t missing_seq : missing_seqs) {
    // ReliabilityManager determines if a NAK was sent recently for this
    // sequence.
    if (!reliability_manager_->is_recently_naked(missing_seq)) {
      send_nak(missing_seq, endpoint);
      reliability_manager_->record_nak_sent(missing_seq); // Mark NAK as sent

      if (++nak_count >= MAX_NAKS_PER_CHECK) {
        std::cout << "[LUMEN] Reached NAK limit for this check ("
                  << MAX_NAKS_PER_CHECK << ")." << std::endl;
        break;
      }
    }
  }
}

// Sends a fully formed LumenPacket over the appropriate UDP transport.
void LumenProtocol::send_packet(const LumenPacket &packet,
                                const udp::endpoint &recipient) {
  std::vector<uint8_t> data =
      packet.to_bytes(); // Serialize the packet including headers, CRC, ETX

  try {
    if (mode_ == ProtocolMode::BASE_STATION) {
      if (!server_) {
        std::cerr << "[ERROR] LumenProtocol (Base): Server pointer is null."
                  << std::endl;
        return;
      }
      server_->send_data(data, recipient);
    } else { // ROVER mode
      if (!client_) {
        std::cerr << "[ERROR] LumenProtocol (Rover): Client pointer is null."
                  << std::endl;
        return;
      }
      // Rover needs to distinguish between sending to the main base station
      // or potentially another specific endpoint (though less common for
      // rover).
      if (recipient == client_->get_base_endpoint()) {
        client_->send_data(
            data); // Use the client's default send (to registered base)
      } else {
        // This case might be used if Rover needs to respond directly to a
        // specific endpoint other than the main base, potentially during
        // discovery or complex scenarios.
        std::cout << "[LUMEN] Rover sending packet to non-default endpoint: "
                  << recipient << std::endl;
        client_->send_data_to(data, recipient);
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] LumenProtocol failed to send packet seq "
              << static_cast<int>(packet.get_header().get_sequence()) << ": "
              << e.what() << std::endl;
  }
}

// Sends an ACK packet (Base Station only).
void LumenProtocol::send_ack(uint8_t seq_to_ack,
                             const udp::endpoint &recipient) {
  if (mode_ != ProtocolMode::BASE_STATION) {
    std::cerr << "[ERROR] Invalid ACK attempt: ROVER mode cannot send ACKs."
              << std::endl;
    return;
  }

  // Record locally that we have acknowledged this sequence. This prevents
  // resending ACKs for duplicate packets.
  reliability_manager_->record_acked_sequence(seq_to_ack, recipient);

  // ACK payload simply contains the sequence number being acknowledged.
  std::vector<uint8_t> ack_payload = {seq_to_ack};

  // Create the ACK packet header. It gets its *own* sequence number.
  uint32_t timestamp = generate_timestamp();
  uint8_t ack_seq =
      current_sequence_++; // Sequence number for this ACK packet itself

  LumenHeader ack_header(LumenHeader::MessageType::ACK,
                         LumenHeader::Priority::HIGH, ack_seq, timestamp,
                         static_cast<uint16_t>(ack_payload.size()));

  LumenPacket ack_packet(ack_header, ack_payload);
  send_packet(ack_packet, recipient);

  std::cout << "[LUMEN] Sent ACK (packet seq " << static_cast<int>(ack_seq)
            << ") for received data seq: " << static_cast<int>(seq_to_ack)
            << " to " << recipient << std::endl;
}

// Sends a NAK packet (Rover only).
void LumenProtocol::send_nak(uint8_t seq_requested,
                             const udp::endpoint &recipient) {
  if (mode_ != ProtocolMode::ROVER) {
    std::cerr
        << "[ERROR] Invalid NAK attempt: BASE_STATION mode cannot send NAKs."
        << std::endl;
    return;
  }

  // NAK payload contains the sequence number the Rover is missing and
  // requesting.
  std::vector<uint8_t> nak_payload = {seq_requested};

  // Create the NAK packet header. It gets its *own* sequence number.
  uint32_t timestamp = generate_timestamp();
  uint8_t nak_seq =
      current_sequence_++; // Sequence number for this NAK packet itself

  LumenHeader nak_header(LumenHeader::MessageType::NAK,
                         LumenHeader::Priority::HIGH, nak_seq, timestamp,
                         static_cast<uint16_t>(nak_payload.size()));

  LumenPacket nak_packet(nak_header, nak_payload);
  // NAK is always sent to the base station endpoint.
  send_packet(nak_packet, client_->get_base_endpoint());

  std::cout << "[LUMEN] Sent NAK (packet seq " << static_cast<int>(nak_seq)
            << ") requesting missing seq: " << static_cast<int>(seq_requested)
            << " to base " << client_->get_base_endpoint() << std::endl;
}

// Generates a 32-bit timestamp (milliseconds since epoch, truncated).
uint32_t LumenProtocol::generate_timestamp() const {
  auto now = std::chrono::system_clock::now();
  auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch())
                    .count();
  return static_cast<uint32_t>(millis & 0xFFFFFFFF); // Mask to fit 32 bits
}

// Callback handler provided to ReliabilityManager for retransmissions.
void LumenProtocol::handle_retransmission(const LumenPacket &packet,
                                          const udp::endpoint &endpoint) {
  std::cout << "[LUMEN] Retransmitting packet seq: "
            << static_cast<int>(packet.get_header().get_sequence()) << " to "
            << endpoint << std::endl;
  // Simply resend the exact same packet.
  send_packet(packet, endpoint);
}