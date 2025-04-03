// src/common/lumen_protocol.cpp

#include "lumen_protocol.hpp"
#include "basic_message.hpp"
#include "configs.hpp"
#include "lumen_header.hpp"
#include "lumen_packet.hpp"
#include "message.hpp"
#include "message_manager.hpp"
#include "reliability_manager.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>

LumenProtocol::LumenProtocol(boost::asio::io_context &io_context,
                             UdpServer &server)
    : mode_(ProtocolMode::BASE_STATION), server_(&server), client_(nullptr),
      current_sequence_(0),
      reliability_manager_(
          std::make_unique<ReliabilityManager>(io_context, true)),
      running_(false), io_context_(io_context) {

  // provide reliabilitymanager with a callback to retransmit packets via this
  // instance
  reliability_manager_->set_retransmit_callback(
      [this](const LumenPacket &packet, const udp::endpoint &endpoint) {
        send_packet(packet, endpoint);
      });

  // set the callback for the underlying udpserver to pass received data up
  server_->set_receive_callback(
      [this](const std::vector<uint8_t> &data, const udp::endpoint &endpoint) {
        handle_udp_data(data, endpoint);
      });

  std::cout << "[LUMEN] Created protocol in BASE_STATION mode" << std::endl;
}

void LumenProtocol::set_timeout_callback(
    ReliabilityManager::TimeoutCallback callback) {
  if (reliability_manager_) {
    reliability_manager_->set_timeout_callback(std::move(callback));
  }
}

LumenProtocol::LumenProtocol(boost::asio::io_context &io_context,
                             UdpClient &client)
    : mode_(ProtocolMode::ROVER), server_(nullptr), client_(&client),
      current_sequence_(0),
      reliability_manager_(
          std::make_unique<ReliabilityManager>(io_context, false)),
      running_(false), io_context_(io_context) {

  // provide reliabilitymanager with a callback to retransmit packets via this
  // instance
  reliability_manager_->set_retransmit_callback(
      [this](const LumenPacket &packet, const udp::endpoint &endpoint) {
        send_packet(packet, endpoint);
      });

  // set the callback for the underlying udpclient to pass received data up
  client_->set_receive_callback(
      [this](const std::vector<uint8_t> &data, const udp::endpoint &sender) {
        handle_udp_data(data, sender);
      });

  std::cout << "[LUMEN] Created protocol in ROVER mode" << std::endl;
}

LumenProtocol::~LumenProtocol() { stop(); }

void LumenProtocol::start() {
  if (running_)
    return;

  running_ = true;

  {
    std::lock_guard<std::mutex> lock(frame_buffers_mutex_);
    frame_buffers_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    endpoint_map_.clear();
  }

  // start the reliability manager's timers and logic
  reliability_manager_->start();

  std::cout << "[LUMEN] Protocol started in "
            << (mode_ == ProtocolMode::BASE_STATION ? "BASE_STATION" : "ROVER")
            << " mode." << std::endl;
}

void LumenProtocol::stop() {
  if (!running_)
    return;

  running_ = false;

  // stop the reliability manager's timers
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

  uint8_t seq = current_sequence_++; // get next sequence number
  uint32_t timestamp = generate_timestamp();
  LumenHeader header(type, priority, seq, timestamp,
                     static_cast<uint16_t>(payload.size()));
  LumenPacket packet(header, payload);

  udp::endpoint target_endpoint;
  if (mode_ == ProtocolMode::BASE_STATION) {
    // base station needs an explicit recipient
    if (recipient.address().is_unspecified()) {
      std::cerr << "[ERROR] No recipient specified for base station mode"
                << std::endl;
      return;
    }
    target_endpoint = recipient;
  } else {
    // rover sends to registered base by default, unless specific recipient
    // provided
    if (!recipient.address().is_unspecified()) {
      target_endpoint = recipient;
    } else {
      target_endpoint = client_->get_base_endpoint();
    }
  }

  // reliabilitymanager tracks packets needing acknowledgement/retransmission
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

void LumenProtocol::reset_sequence_number() {
  current_sequence_ = 0; // reset sequence counter
  if (reliability_manager_) {
    reliability_manager_->reset_state(); // reset reliability state
  }
  {
    std::lock_guard<std::mutex> lock(frame_buffers_mutex_);
    frame_buffers_.clear();
  }
  std::cout
      << "[LUMEN] Sequence number reset to 0 and reliability state cleared."
      << std::endl;
}

// helper to create a consistent string key from an endpoint for map lookups
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
  if (!running_ || data.empty())
    return;

  // check if it's a lumen packet (starts with stx)
  if (data[0] == LUMEN_STX) {
    std::string endpoint_key = get_endpoint_key(endpoint);
    {
      std::lock_guard<std::mutex> lock(endpoint_mutex_);
      endpoint_map_[endpoint_key] = endpoint;
    }
    {
      std::lock_guard<std::mutex> lock(frame_buffers_mutex_);
      frame_buffers_[endpoint_key].insert(frame_buffers_[endpoint_key].end(),
                                          data.begin(), data.end());
    }
    // attempt to process complete lumen packets from the buffer
    process_frame_buffer(endpoint_key, endpoint);
    return;
  }
  // if not lumen, check if it's raw json (starts with '{')
  else if (data[0] == '{') {
    std::cout << "[LUMEN] Detected potential raw JSON message from " << endpoint
              << std::endl;
    std::string json_str(data.begin(), data.end());

    try {
      if (Message::is_valid_json(json_str)) {
        auto message = Message::deserialise(json_str);
        if (message) {
          std::cout
              << "[LUMEN] Successfully deserialized raw JSON message type: "
              << message->get_type() << std::endl;

          // repackage and send to messagemanager via existing callback
          // mechanism
          LumenHeader dummy_header( // create a dummy header
              LumenHeader::MessageType::DATA, LumenHeader::Priority::MEDIUM, 0,
              generate_timestamp(), static_cast<uint16_t>(data.size()));

          std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                             const udp::endpoint &)>
              msg_mgr_callback;
          {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            msg_mgr_callback = message_callback_;
          }

          if (msg_mgr_callback) {
            // pass original raw binary data (json) as payload
            msg_mgr_callback(data, dummy_header, endpoint);
          } else {
            std::cerr << "[LUMEN] Warning: No MessageManager callback set for "
                         "raw JSON."
                      << std::endl;
          }
          return; // successfully processed as raw json
        } else {
          std::cerr << "[LUMEN] Valid JSON detected but failed "
                       "Message::deserialise from "
                    << endpoint << std::endl;
        }
      } else {
        std::cerr << "[LUMEN] Received data from " << endpoint
                  << " starting with '{' but failed JSON validation."
                  << std::endl;
      }
    } catch (const std::exception &e) {
      std::cerr << "[ERROR] Failed to process raw JSON from " << endpoint
                << ": " << e.what() << std::endl;
    }
    // fall through if json processing failed
  }

  // fallback: treat data as a raw string and wrap in basicmessage
  std::cout << "[LUMEN] Received non-Lumen, non-JSON data from " << endpoint
            << ". Wrapping in BasicMessage." << std::endl;

  try {
    std::string content_string(data.begin(), data.end());
    std::string unknown_sender_id = "Unknown:" + get_endpoint_key(endpoint);
    BasicMessage basic_msg_obj(content_string, unknown_sender_id);

    std::string json_payload_str = basic_msg_obj.serialise();
    std::vector<uint8_t> binary_payload(json_payload_str.begin(),
                                        json_payload_str.end());

    LumenHeader dummy_header(
        LumenHeader::MessageType::DATA, LumenHeader::Priority::MEDIUM, 0,
        generate_timestamp(), static_cast<uint16_t>(binary_payload.size()));

    std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                       const udp::endpoint &)>
        msg_mgr_callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      msg_mgr_callback = message_callback_;
    }

    if (msg_mgr_callback) {
      msg_mgr_callback(binary_payload, dummy_header, endpoint);
    } else {
      std::cerr << "[LUMEN] Warning: No MessageManager callback set to handle "
                   "wrapped BasicMessage."
                << std::endl;
    }

  } catch (const std::exception &e) {
    std::cerr << "[LUMEN] Error wrapping raw data into BasicMessage from "
              << endpoint << ": " << e.what() << std::endl;
  }
}

void LumenProtocol::process_frame_buffer(const std::string &endpoint_key,
                                         const udp::endpoint &endpoint) {
  std::lock_guard<std::mutex> lock(frame_buffers_mutex_);

  if (frame_buffers_.find(endpoint_key) == frame_buffers_.end()) {
    return;
  }

  auto &buffer = frame_buffers_[endpoint_key];

  // process as many complete packets as possible from buffer start
  while (!buffer.empty()) {
    // lumenpacket::from_bytes checks stx, header, length, crc, etx
    auto packet_opt = LumenPacket::from_bytes(buffer);

    // if not enough data or packet invalid
    if (!packet_opt) {
      // try parsing just header to get expected size
      auto header_opt = LumenHeader::from_bytes(buffer);
      if (header_opt) {
        // header parsable, maybe payload/crc/etx bad
        LumenHeader header = *header_opt;
        size_t expected_total_size =
            LUMEN_HEADER_SIZE + header.get_payload_length() + 2;

        // if we have enough data for declared size, discard chunk to avoid
        // re-parsing corrupt packet
        if (buffer.size() >= expected_total_size) {
          std::cerr << "[LUMEN] Discarding " << expected_total_size
                    << " bytes from buffer for " << endpoint_key
                    << " due to packet parse failure (likely CRC/ETX error)."
                    << std::endl;
          buffer.erase(buffer.begin(), buffer.begin() + expected_total_size);
          continue; // try processing rest of buffer
        } else {
          // not enough data for declared size, wait for more
          break;
        }
      } else {
        // even header is bad (missing stx, too short)
        // fallback: find next stx and discard before it
        size_t next_stx_pos = std::string::npos;
        for (size_t i = 1; i < buffer.size(); ++i) {
          if (buffer[i] == LUMEN_STX) {
            next_stx_pos = i;
            break;
          }
        }

        if (next_stx_pos != std::string::npos) {
          std::cerr << "[LUMEN] Discarding " << next_stx_pos
                    << " bytes from buffer for " << endpoint_key
                    << " due to unparsable header/data before next STX."
                    << std::endl;
          buffer.erase(buffer.begin(), buffer.begin() + next_stx_pos);
          continue; // try processing from next stx
        } else {
          // no stx found or only at start but header failed
          if (buffer.size() > MAX_FRAME_BUFFER_SIZE / 2) { // heuristic
            std::cerr << "[LUMEN] No valid STX found in large buffer for "
                      << endpoint_key << ". Clearing " << buffer.size()
                      << " bytes." << std::endl;
            buffer.clear();
          }
          // if buffer small and no next stx, wait for more data
          break;
        }
      }
    }

    // packet successfully parsed here
    LumenPacket packet = *packet_opt;
    size_t packet_size = packet.total_size();

    // remove processed packet bytes from buffer
    if (packet_size <= buffer.size()) {
      buffer.erase(buffer.begin(), buffer.begin() + packet_size);
    } else {
      // indicates logic error if from_bytes succeeded but sizes mismatch
      std::cerr << "[LUMEN] Internal error: Packet size (" << packet_size
                << ") > buffer size (" << buffer.size()
                << ") after successful parse for " << endpoint_key
                << ". Clearing buffer." << std::endl;
      buffer.clear(); // prevent potential infinite loops
      break;
    }

    // pass validated packet for processing
    process_complete_packet(packet, endpoint);

  } // end while loop

  // prevent buffer growing indefinitely
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

  std::cout << "[LUMEN] Processing packet seq: " << static_cast<int>(seq)
            << ", type: " << static_cast<int>(static_cast<uint8_t>(type))
            << ", size: " << payload.size() << " from " << endpoint
            << std::endl;

  reliability_manager_->record_received_sequence(seq, endpoint);

  // --- handle control packets (ack/nak) ---
  if (type == LumenHeader::MessageType::ACK) {
    // rover expects acks from base station
    if (mode_ == ProtocolMode::ROVER && payload.size() >= 1) {
      uint8_t acked_seq = payload[0]; // ack payload contains seq being acked
      std::cout << "[LUMEN] Processing ACK for original seq: "
                << static_cast<int>(acked_seq) << std::endl;
      reliability_manager_->process_ack(acked_seq);
    } else if (mode_ == ProtocolMode::BASE_STATION) {
      std::cout << "[LUMEN] Warning: Ignoring unexpected ACK received in "
                   "BASE_STATION mode from "
                << endpoint << std::endl;
    }
    return; // acks processed here
  }

  if (type == LumenHeader::MessageType::NAK) {
    // base station expects naks from rover
    if (mode_ == ProtocolMode::BASE_STATION && payload.size() >= 1) {
      uint8_t requested_seq =
          payload[0]; // nak payload contains seq being requested
      std::cout << "[LUMEN] Processing NAK for missing seq: "
                << static_cast<int>(requested_seq) << std::endl;
      reliability_manager_->process_nak(
          requested_seq); // reliabilitymanager handles retransmission
    } else if (mode_ == ProtocolMode::ROVER) {
      std::cout << "[LUMEN] Warning: Ignoring unexpected NAK received in ROVER "
                   "mode from "
                << endpoint << std::endl;
    }
    return; // naks processed here
  }
  // --- end control packet handling ---

  bool forward_payload = true; // assume forward unless known duplicate

  if (mode_ == ProtocolMode::BASE_STATION) {
    bool is_active = session_active_.load(); // check session state flag

    // check if we already know this sequence number
    bool already_acked =
        reliability_manager_->has_acked_sequence(seq, endpoint);

    if (is_active) {
      // session active: send ack unless duplicate, resend ack if duplicate
      if (!already_acked) {
        reliability_manager_->record_acked_sequence(
            seq, endpoint);      // record ack first time
        send_ack(seq, endpoint); // send ack
      } else {
        // duplicate packet received while active
        std::cout << "[LUMEN] Duplicate packet seq: " << static_cast<int>(seq)
                  << " from " << endpoint << " (already ACKed). Resending ACK."
                  << std::endl;
        send_ack(seq, endpoint); // resend ack in case first was lost
        forward_payload = false; // do not forward payload for duplicates
      }
    } else {
      // session not active: do not send ack for non-control packets
      std::cout << "[LUMEN] Suppressing ACK for seq: " << static_cast<int>(seq)
                << " from " << endpoint << ". Base Station session not active."
                << std::endl;
      if (already_acked) {
        forward_payload =
            false; // don't forward duplicates even if inactive now
        std::cout << "[LUMEN] Duplicate packet seq: " << static_cast<int>(seq)
                  << " received while inactive. Not forwarding payload."
                  << std::endl;
      }
      // note: if non-duplicate packet arrives while inactive, we don't ack, but
      // might forward payload
    }
  }

  // rover checks for gaps periodically after receiving packets
  if (mode_ == ProtocolMode::ROVER) {
    static uint8_t check_counter = 0;
    if (++check_counter % 5 == 0) { // check roughly every 5 packets
      check_sequence_gaps(endpoint);
    }
  }

  // --- forward payload to upper layer (messagemanager) ---
  if (forward_payload) {
    std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                       const udp::endpoint &)>
        callback_copy;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback_copy = message_callback_;
    }

    if (callback_copy) {
      // pass payload, header, sender to messagemanager
      callback_copy(payload, header, endpoint);
    } else {
      std::cerr << "[LUMEN] Warning: No message callback set. Discarding "
                   "payload for packet seq: "
                << static_cast<int>(seq) << std::endl;
    }
  }
}

// checks for missing sequence numbers (rover only)
void LumenProtocol::check_sequence_gaps(const udp::endpoint &endpoint) {
  // ask reliabilitymanager for sequences deemed missing
  std::vector<uint8_t> missing_seqs =
      reliability_manager_->get_missing_sequences(endpoint);

  // limit number of naks sent at once
  const int MAX_NAKS_PER_CHECK = 3;
  int nak_count = 0;

  for (uint8_t missing_seq : missing_seqs) {
    // reliabilitymanager determines if nak sent recently
    if (!reliability_manager_->is_recently_naked(missing_seq)) {
      send_nak(missing_seq, endpoint);
      reliability_manager_->record_nak_sent(missing_seq); // mark nak as sent

      if (++nak_count >= MAX_NAKS_PER_CHECK) {
        std::cout << "[LUMEN] Reached NAK limit for this check ("
                  << MAX_NAKS_PER_CHECK << ")." << std::endl;
        break;
      }
    }
  }
}

// sends a fully formed lumenpacket over the appropriate udp transport
void LumenProtocol::send_packet(const LumenPacket &packet,
                                const udp::endpoint &recipient) {
  std::vector<uint8_t> data = packet.to_bytes(); // serialize packet

  try {
    if (mode_ == ProtocolMode::BASE_STATION) {
      if (!server_) {
        std::cerr << "[ERROR] LumenProtocol (Base): Server pointer is null."
                  << std::endl;
        return;
      }
      server_->send_data(data, recipient);
    } else { // rover mode
      if (!client_) {
        std::cerr << "[ERROR] LumenProtocol (Rover): Client pointer is null."
                  << std::endl;
        return;
      }
      // rover distinguishes sending to base vs another rover/endpoint
      bool sending_to_base = false;
      try {
        sending_to_base = (recipient == client_->get_base_endpoint());
      } catch (const std::runtime_error &) {
        // handle case where base endpoint might not be resolved yet
        sending_to_base = false;
      }

      if (sending_to_base) {
        client_->send_data(
            data); // use client's default send (to registered base)
      } else {
        client_->send_data_to(data,
                              recipient); // send to specific non-base endpoint
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] LumenProtocol failed to send packet seq "
              << static_cast<int>(packet.get_header().get_sequence()) << " to "
              << recipient << ": " << e.what() << std::endl;
  }
}

// sends an ack packet (base station only)
void LumenProtocol::send_ack(uint8_t seq_to_ack,
                             const udp::endpoint &recipient) {
  if (mode_ != ProtocolMode::BASE_STATION) {
    std::cerr << "[ERROR] Invalid ACK attempt: ROVER mode cannot send ACKs."
              << std::endl;
    return;
  }

  // record locally that we have acknowledged this sequence
  reliability_manager_->record_acked_sequence(seq_to_ack, recipient);

  // ack payload contains sequence number being acknowledged
  std::vector<uint8_t> ack_payload = {seq_to_ack};

  // create ack packet header with its own sequence number
  uint32_t timestamp = generate_timestamp();
  uint8_t ack_seq =
      current_sequence_++; // sequence number for this ack packet itself

  LumenHeader ack_header(LumenHeader::MessageType::ACK,
                         LumenHeader::Priority::HIGH, ack_seq, timestamp,
                         static_cast<uint16_t>(ack_payload.size()));

  LumenPacket ack_packet(ack_header, ack_payload);
  send_packet(ack_packet, recipient);

  std::cout << "[LUMEN] Sent ACK (packet seq " << static_cast<int>(ack_seq)
            << ") for received data seq: " << static_cast<int>(seq_to_ack)
            << " to " << recipient << std::endl;
}

// sends a nak packet (rover only)
void LumenProtocol::send_nak(uint8_t seq_requested, const udp::endpoint &) {
  if (mode_ != ProtocolMode::ROVER) {
    std::cerr
        << "[ERROR] Invalid NAK attempt: BASE_STATION mode cannot send NAKs."
        << std::endl;
    return;
  }

  // ensure client and base endpoint are valid
  if (!client_) {
    std::cerr << "[LUMEN] Cannot send NAK: Client pointer is null."
              << std::endl;
    return;
  }
  udp::endpoint base_endpoint;
  try {
    base_endpoint = client_->get_base_endpoint();
  } catch (const std::runtime_error &e) {
    std::cerr << "[LUMEN] Cannot send NAK: Failed to get base endpoint: "
              << e.what() << std::endl;
    return;
  }

  // nak payload contains sequence number rover is missing
  std::vector<uint8_t> nak_payload = {seq_requested};

  // create nak packet header with its own sequence number
  uint32_t timestamp = generate_timestamp();
  uint8_t nak_seq =
      current_sequence_++; // sequence number for this nak packet itself

  LumenHeader nak_header(LumenHeader::MessageType::NAK,
                         LumenHeader::Priority::HIGH, nak_seq, timestamp,
                         static_cast<uint16_t>(nak_payload.size()));

  LumenPacket nak_packet(nak_header, nak_payload);
  // nak always sent to base station endpoint
  send_packet(nak_packet, base_endpoint);

  std::cout << "[LUMEN] Sent NAK (packet seq " << static_cast<int>(nak_seq)
            << ") requesting missing seq: " << static_cast<int>(seq_requested)
            << " to base " << base_endpoint << std::endl;
}

// generates a 32-bit timestamp (milliseconds since epoch, truncated)
uint32_t LumenProtocol::generate_timestamp() const {
  auto now = std::chrono::system_clock::now();
  auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch())
                    .count();
  return static_cast<uint32_t>(millis & 0xFFFFFFFF); // mask to fit 32 bits
}

// callback handler provided to reliabilitymanager for retransmissions
void LumenProtocol::handle_retransmission(const LumenPacket &packet,
                                          const udp::endpoint &endpoint) {
  std::cout << "[LUMEN] Retransmitting packet seq: "
            << static_cast<int>(packet.get_header().get_sequence()) << " to "
            << endpoint << std::endl;
  // simply resend the exact same packet
  send_packet(packet, endpoint);
}

void LumenProtocol::set_session_active(bool active) {
  session_active_.store(active);
  std::cout << "[LUMEN] Session active flag set to: "
            << (active ? "true" : "false") << std::endl;
}