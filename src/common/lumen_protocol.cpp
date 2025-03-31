#include "lumen_protocol.hpp"
#include "configs.hpp"
#include "lumen_header.hpp"
#include "lumen_packet.hpp"
#include "reliability_manager.hpp"

#include <atomic>
#include <iostream>
#include <mutex>
#include <unordered_map>

// constructor for base station
LumenProtocol::LumenProtocol(boost::asio::io_context &io_context,
                             UdpServer &server, bool send_acks, bool use_nak)
    : mode_(Mode::SERVER), server_(&server), client_(nullptr),
      send_sequence_(0), incoming_expected_sequence_(0),
      reliability_manager_(std::make_unique<ReliabilityManager>(io_context)),
      send_acks_(send_acks), use_nak_(use_nak), running_(false),
      io_context_(io_context), buffer_sender_endpoint_(udp::endpoint()) {

  // set up retransmission callback
  reliability_manager_->set_retransmit_callback(
      [this](const LumenPacket &packet, const udp::endpoint &endpoint) {
        handle_retransmission(packet, endpoint);
      });

  // set up callback to handle UDP data
  server_->set_receive_callback(
      [this](const std::vector<uint8_t> &data, const udp::endpoint &endpoint) {
        handle_udp_data(data, endpoint);
      });
}

// constructor for rover
LumenProtocol::LumenProtocol(boost::asio::io_context &io_context,
                             UdpClient &client, bool send_acks, bool use_nak)
    : mode_(Mode::CLIENT), server_(nullptr), client_(&client),
      send_sequence_(0), incoming_expected_sequence_(0),
      reliability_manager_(std::make_unique<ReliabilityManager>(io_context)),
      send_acks_(send_acks), use_nak_(use_nak), running_(false),
      io_context_(io_context), buffer_sender_endpoint_(udp::endpoint()) {

  // set up retransmission callback
  reliability_manager_->set_retransmit_callback(
      [this](const LumenPacket &packet, const udp::endpoint &endpoint) {
        handle_retransmission(packet, endpoint);
      });

  // set up callback to handle UDP data
  client_->set_receive_callback([this](const std::vector<uint8_t> &data) {
    handle_udp_data(data, client_->get_base_endpoint());
  });
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

  // start reliability manager
  reliability_manager_->start();

  std::cout << "[LUMEN] Protocol started in "
            << (mode_ == Mode::SERVER ? "server" : "client") << " mode."
            << std::endl;
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

  // Use send_sequence_ for outgoing packets.
  uint8_t seq = send_sequence_.fetch_add(1);
  uint32_t timestamp = generate_timestamp();
  LumenHeader header(type, priority, seq, timestamp,
                     static_cast<uint16_t>(payload.size()));
  LumenPacket packet(header, payload);

  udp::endpoint target_endpoint;
  if (mode_ == Mode::SERVER) {
    if (recipient.address().is_unspecified()) {
      std::cerr << "[ERROR] No recipient specified for server mode"
                << std::endl;
      return;
    }
    target_endpoint = recipient;
  } else {
    target_endpoint = client_->get_base_endpoint();
  }

  reliability_manager_->add_send_packet(seq, packet, target_endpoint);
  send_packet(packet, target_endpoint);

  std::cout << "[LUMEN] Sent packet with seq: " << static_cast<int>(seq)
            << ", type: " << static_cast<int>(static_cast<uint8_t>(type))
            << ", size: " << payload.size() << " to " << target_endpoint
            << std::endl;
}

void LumenProtocol::set_message_callback(
    std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                       const udp::endpoint &)>
        callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  message_callback_ = std::move(callback);
}

uint8_t LumenProtocol::get_current_sequence() const {
  return send_sequence_.load();
}

void LumenProtocol::handle_udp_data(const std::vector<uint8_t> &data,
                                    const udp::endpoint &endpoint) {
  if (!running_)
    return;

  std::string sender_key =
      endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
  // add data to frame buffer
  {
    std::lock_guard<std::mutex> lock(frame_buffers_mutex_);
    // append the received data to the sender-specific buffer.
    frame_buffers_[sender_key].insert(frame_buffers_[sender_key].end(),
                                      data.begin(), data.end());
  }

  // process frame buffer to extract complete packets
  process_frame_buffer_for_sender(sender_key, endpoint);
}

void LumenProtocol::process_frame_buffer_for_sender(
    const std::string &sender_key, const udp::endpoint &endpoint) {
  std::lock_guard<std::mutex> lock(frame_buffers_mutex_);
  auto &buffer = frame_buffers_[sender_key];

  // Discard any bytes before the start-of-transmission marker (STX)
  while (!buffer.empty() && static_cast<uint8_t>(buffer.front()) != LUMEN_STX) {
    buffer.erase(buffer.begin());
  }

  // process complete packets in the sender's specific buffer
  while (!buffer.empty()) {
    auto packet_opt = LumenPacket::from_bytes(buffer);
    if (!packet_opt) {
      // there's no complete packet yet, leave the partial data
      break;
    }
    LumenPacket packet = *packet_opt;
    uint16_t packet_size = packet.total_size();
    if (packet_size <= buffer.size()) {
      buffer.erase(buffer.begin(), buffer.begin() + packet_size);
    } else {
      break;
    }
    // process the complete extracted packet
    process_complete_packet(packet, endpoint);
  }

  // prevent buffer overflow
  if (buffer.size() > MAX_FRAME_BUFFER_SIZE) {
    std::cerr << "[LUMEN] Frame buffer overflow for sender " << sender_key
              << ", clearing buffer" << std::endl;
    buffer.clear();
  }
}

void LumenProtocol::process_complete_packet(const LumenPacket &packet,
                                            const udp::endpoint &endpoint) {
  const LumenHeader &header = packet.get_header();
  const std::vector<uint8_t> &payload = packet.get_payload();
  LumenHeader::MessageType type = header.get_type();
  uint8_t seq = header.get_sequence();

  std::cout << "[LUMEN] Received packet with seq: " << static_cast<int>(seq)
            << ", type: " << static_cast<int>(static_cast<uint8_t>(type))
            << ", size: " << payload.size() << " from " << endpoint
            << std::endl;

  // process control packets separately
  if (type == LumenHeader::MessageType::ACK) {
    if (!payload.empty()) {
      uint8_t acked_seq = payload[0];
      reliability_manager_->process_ack(acked_seq);
    }
    // do not update incoming_expected_sequence_ for control packets.
    return;
  } else if (type == LumenHeader::MessageType::NAK) {
    if (!payload.empty()) {
      uint8_t requested_seq = payload[0];
      reliability_manager_->process_nak(requested_seq);
    }
    return;
  }

  // 2. for non–control messages, enforce in–order delivery
  if (seq != incoming_expected_sequence_.load()) {
    std::cout << "[LUMEN] Out-of-order packet: expected seq "
              << static_cast<int>(incoming_expected_sequence_.load())
              << " but got " << static_cast<int>(seq) << std::endl;
    if (use_nak_) {
      send_nak(incoming_expected_sequence_.load(), endpoint);
    }
    return; // drop out-of-order packet
  }
  // the packet is in order: update expected sequence (wrap-around)
  incoming_expected_sequence_ = (incoming_expected_sequence_.load() + 1) & 0xFF;

  // 3. Deliver the packet payload to the callback
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

void LumenProtocol::send_packet(const LumenPacket &packet,
                                const udp::endpoint &recipient) {
  // serialize packet to bytes
  std::vector<uint8_t> data = packet.to_bytes();

  try {
    // send with appropriate mode
    if (mode_ == Mode::SERVER) {
      server_->send_data(data, recipient);
    } else {
      client_->send_data(data);
    }
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Failed to send packet: " << e.what() << std::endl;
  }
}

void LumenProtocol::send_ack(uint8_t seq, const udp::endpoint &recipient) {
  // create ACK payload with sequence number (echoing the received seq)
  std::vector<uint8_t> ack_payload = {seq};
  uint32_t timestamp = generate_timestamp();
  // Use seq directly for the ACK header.
  LumenHeader ack_header(LumenHeader::MessageType::ACK,
                         LumenHeader::Priority::HIGH,
                         seq, // echo the sequence being acknowledged
                         timestamp, static_cast<uint16_t>(ack_payload.size()));
  LumenPacket ack_packet(ack_header, ack_payload);
  send_packet(ack_packet, recipient);

  std::cout << "[LUMEN] Sent ACK for seq: " << static_cast<int>(seq) << " to "
            << recipient << std::endl;
}

void LumenProtocol::send_nak(uint8_t seq, const udp::endpoint &recipient) {
  // create NAK payload with the expected sequence number.
  std::vector<uint8_t> nak_payload = {seq};
  uint32_t timestamp = generate_timestamp();
  LumenHeader nak_header(LumenHeader::MessageType::NAK,
                         LumenHeader::Priority::HIGH,
                         seq, // use expected sequence number
                         timestamp, static_cast<uint16_t>(nak_payload.size()));
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
