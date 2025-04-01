// include/common/lumen_protocol.hpp

#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

#include "../base/udp_server.hpp"
#include "../rover/udp_client.hpp"

#include "lumen_header.hpp"
#include "lumen_packet.hpp"
#include "reliability_manager.hpp"

using boost::asio::ip::udp;

class LumenProtocol {
public:
  enum class ProtocolMode {
    BASE_STATION, // Sends ACKs, expects NAKs
    ROVER         // Sends NAKs, expects ACKs
  };

  // constructor for base station
  LumenProtocol(boost::asio::io_context &io_context, UdpServer &server);

  // constructor for rover
  LumenProtocol(boost::asio::io_context &io_context, UdpClient &client);

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
  void handle_udp_data(const std::vector<uint8_t> &data,
                       const udp::endpoint &endpoint);

  void process_complete_packet(const LumenPacket &packet,
                               const udp::endpoint &endpoint);

  // send a raw packet
  void send_packet(const LumenPacket &packet, const udp::endpoint &recipient);

  // handle acks and naks
  void send_ack(uint8_t seq, const udp::endpoint &recipient);
  void send_nak(uint8_t seq, const udp::endpoint &recipient);

  // generate a timestamp for lumen header
  uint32_t generate_timestamp() const;

  // handle retransmission requests from reliability manager
  void handle_retransmission(const LumenPacket &packet,
                             const udp::endpoint &endpoint);

  // Process frame buffers
  void process_frame_buffer(const std::string &endpoint_key,
                            const udp::endpoint &endpoint);

  // Get endpoint key string
  std::string get_endpoint_key(const udp::endpoint &endpoint) const;

  // Check for sequence gap and send NAK if needed (rover only)
  void check_sequence_gaps(const udp::endpoint &endpoint);

  // references to lower layers
  ProtocolMode mode_;
  UdpServer *server_;
  UdpClient *client_;

  // frame buffer for reassembly - keyed by endpoint
  std::unordered_map<std::string, std::vector<uint8_t>> frame_buffers_;

  // sequence number management
  std::atomic<uint8_t> current_sequence_;

  // Track last sender endpoint per buffer
  std::unordered_map<std::string, udp::endpoint> endpoint_map_;

  // reliability management
  std::unique_ptr<ReliabilityManager> reliability_manager_;

  // callback for received messages
  std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                     const udp::endpoint &)>
      message_callback_;

  // Mutex for thread safety
  std::mutex frame_buffers_mutex_;
  std::mutex callback_mutex_;
  std::mutex endpoint_mutex_;

  bool running_;

  // IO context reference
  boost::asio::io_context &io_context_;

  // Sliding window for sequence number tracking
  static constexpr uint8_t WINDOW_SIZE = 32;
};