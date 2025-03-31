// include/common/lumen_protocol.hpp

#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <memory>
#include <vector>

#include "../base/udp_server.hpp"
#include "../rover/udp_client.hpp"

#include "lumen_header.hpp"
#include "lumen_packet.hpp"
#include "reliability_manager.hpp"
#include <unordered_map>

using boost::asio::ip::udp;

class LumenProtocol {
public:
  // constructor for base station
  LumenProtocol(boost::asio::io_context &io_context, UdpServer &server,
                bool send_acks = true, bool use_sack = false);

  // constructor for rover
  LumenProtocol(boost::asio::io_context &io_context, UdpClient &client,
                bool send_acks = false, bool use_sack = true);

  // destructor
  ~LumenProtocol();

  void start();
  void stop();

  void send_message(const std::vector<uint8_t> &payload,
                    LumenHeader::MessageType type,
                    LumenHeader::Priority priority,
                    const udp::endpoint &recipient = udp::endpoint());

  void set_message_callback(
      std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                         const udp::endpoint &)>
          callback);

  // Get current sequence number
  uint8_t get_current_sequence() const;

private:
  enum class Mode { SERVER, CLIENT };

  void handle_udp_data(const std::vector<uint8_t> &data,
                       const udp::endpoint &endpoint);

  void process_complete_packet(const LumenPacket &packet,
                               const udp::endpoint &endpoint);

  // send a raw packet
  void send_packet(const LumenPacket &packet, const udp::endpoint &recipient);

  // handle acks and sacks
  void send_ack(uint8_t seq, const udp::endpoint &recipient);
  void send_sack(const udp::endpoint &recipient);

  // generate a timestamp for lumen header
  uint32_t generate_timestamp() const;

  // handle retransmission requests from reliability manager
  void handle_retransmission(const LumenPacket &packet,
                             const udp::endpoint &endpoint);

  // references to lower layers
  Mode mode_;
  UdpServer *server_;
  UdpClient *client_;

  // frame buffer for reassembly
  std::unordered_map<std::string, std::vector<uint8_t>> frame_buffers_;

  // sequence number management
  std::atomic<uint8_t> current_sequence_;

  // reliability management
  std::unique_ptr<ReliabilityManager> reliability_manager_;

  // callback for received messages
  std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                     const udp::endpoint &)>
      message_callback_;

  std::mutex frame_buffers_mutex_;
  std::mutex callback_mutex_;

  bool send_acks_;
  bool use_sack_;

  bool running_;

  // IO context reference
  boost::asio::io_context &io_context_;

  void process_frame_buffer_for_sender(const std::string &sender_key,
                                       const udp::endpoint &endpoint);

  // Endpoint tracking for frame buffer
  udp::endpoint buffer_sender_endpoint_;
};