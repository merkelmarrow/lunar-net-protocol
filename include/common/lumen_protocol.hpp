// include/common/lumen_protocol.hpp

#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../base/udp_server.hpp"
#include "../rover/udp_client.hpp"

#include "lumen_header.hpp"
#include "lumen_packet.hpp"
#include "reliability_manager.hpp"

using boost::asio::ip::udp;

// implements the lumen custom protocol layer
class LumenProtocol {
public:
  // defines the operational mode, affecting reliability behavior
  enum class ProtocolMode { BASE_STATION, ROVER };
  void set_timeout_callback(ReliabilityManager::TimeoutCallback callback);

  // constructor for base_station mode
  LumenProtocol(boost::asio::io_context &io_context, UdpServer &server);

  // constructor for rover mode
  LumenProtocol(boost::asio::io_context &io_context, UdpClient &client);

  ~LumenProtocol();

  LumenProtocol(const LumenProtocol &) = delete;
  LumenProtocol &operator=(const LumenProtocol &) = delete;

  void start();

  void stop();

  void reset_sequence_number();

  // sends a payload through the lumen protocol
  void send_message(const std::vector<uint8_t> &payload,
                    LumenHeader::MessageType type,
                    LumenHeader::Priority priority,
                    const udp::endpoint &recipient = udp::endpoint());

  // sets the callback function invoked when a complete lumen packet payload is
  // received
  void set_message_callback(
      std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                         const udp::endpoint &)>
          callback);

  uint8_t get_current_sequence() const;

  void set_session_active(bool active);

private:
  // internal callback handler for raw data received from the udp layer
  void handle_udp_data(const std::vector<uint8_t> &data,
                       const udp::endpoint &endpoint);

  // processes a fully parsed and validated lumenpacket
  void process_complete_packet(const LumenPacket &packet,
                               const udp::endpoint &endpoint);

  // sends a pre-constructed lumenpacket over the appropriate udp transport
  void send_packet(const LumenPacket &packet, const udp::endpoint &recipient);

  // sends an ack packet (base station mode only)
  void send_ack(uint8_t seq_to_ack, const udp::endpoint &recipient);

  // sends a nak packet (rover mode only)
  void send_nak(uint8_t seq_requested, const udp::endpoint &recipient);

  // generates a 32-bit timestamp
  uint32_t generate_timestamp() const;

  // callback function passed to reliabilitymanager to handle retransmissions
  void handle_retransmission(const LumenPacket &packet,
                             const udp::endpoint &endpoint);

  // attempts to parse complete lumenpackets from the raw byte buffer
  void process_frame_buffer(const std::string &endpoint_key,
                            const udp::endpoint &endpoint);

  // creates a unique string key from a udp endpoint address and port
  std::string get_endpoint_key(const udp::endpoint &endpoint) const;

  // checks for missing sequence numbers (rover mode only)
  void check_sequence_gaps(const udp::endpoint &endpoint);

  // --- member variables ---

  ProtocolMode mode_;
  UdpServer *server_;
  UdpClient *client_;

  // buffer for incoming udp data, keyed by endpoint string
  std::unordered_map<std::string, std::vector<uint8_t>> frame_buffers_;

  // atomically incremented sequence number for outgoing packets
  std::atomic<uint8_t> current_sequence_;

  // maps endpoint string keys back to actual endpoint objects
  std::unordered_map<std::string, udp::endpoint> endpoint_map_;

  // manages reliability logic
  std::unique_ptr<ReliabilityManager> reliability_manager_;

  // callback function pointer (set by messagemanager)
  std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                     const udp::endpoint &)>
      message_callback_;

  std::mutex frame_buffers_mutex_;
  std::mutex callback_mutex_;
  std::mutex endpoint_mutex_;

  std::atomic<bool> running_;
  std::atomic<bool> session_active_ = false;

  boost::asio::io_context &io_context_;
};