// include/common/lumen_protocol.hpp

#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <functional> // For std::function
#include <memory>
#include <mutex>
#include <string> // For std::string
#include <unordered_map>
#include <vector>

// Forward declarations to avoid circular dependencies if possible,
// but full includes are needed for member types UdpServer* and UdpClient*.
#include "../base/udp_server.hpp"
#include "../rover/udp_client.hpp"

#include "lumen_header.hpp"
#include "lumen_packet.hpp"
#include "reliability_manager.hpp"

using boost::asio::ip::udp;

/**
 * @class LumenProtocol
 * @brief Implements the LUMEN custom protocol layer responsible for packet
 * framing, reliability (via ReliabilityManager), and routing between the
 * MessageManager and the UDP transport.
 *
 * This class acts as an intermediary between the high-level message objects
 * (managed by MessageManager) and the low-level UDP byte transmission (handled
 * by UdpServer/UdpClient). It operates in one of two modes: BASE_STATION or
 * ROVER, which dictates its role in the ACK/NAK reliability scheme.
 *
 * Responsibilities include:
 * - Framing: Adding LUMEN headers (STX, type, priority, sequence, timestamp,
 * length) and trailers (CRC, ETX) to outgoing payloads.
 * - Reassembly: Buffering incoming UDP data per endpoint and parsing complete
 * LUMEN packets.
 * - Reliability Coordination: Interacting with ReliabilityManager to track sent
 * packets, handle ACKs/NAKs, trigger retransmissions (Base via NAK, Rover via
 * timeout/ACK), and detect sequence gaps (Rover).
 * - Routing: Directing outgoing packets to the correct UDP transport
 * (Server/Client) and recipient. Passing validated incoming payloads up to the
 * MessageManager via a callback.
 * - Raw JSON Handling: Optionally processing raw JSON messages received without
 * LUMEN headers.
 */
class LumenProtocol {
public:
  /**
   * @enum ProtocolMode
   * @brief Defines the operational mode, affecting reliability behavior.
   */
  enum class ProtocolMode {
    BASE_STATION, ///< Operates as a server, sends ACKs for received data,
                  ///< expects NAKs for retransmissions.
    ROVER ///< Operates as a client, expects ACKs for sent data, sends NAKs for
          ///< missing data.
  };
  void set_timeout_callback(ReliabilityManager::TimeoutCallback callback);

  /**
   * @brief Constructor for BASE_STATION mode.
   * @param io_context The Boost.Asio io_context for asynchronous operations.
   * @param server A reference to the UdpServer instance handling UDP
   * communication.
   */
  LumenProtocol(boost::asio::io_context &io_context, UdpServer &server);

  /**
   * @brief Constructor for ROVER mode.
   * @param io_context The Boost.Asio io_context for asynchronous operations.
   * @param client A reference to the UdpClient instance handling UDP
   * communication.
   */
  LumenProtocol(boost::asio::io_context &io_context, UdpClient &client);

  /**
   * @brief Destructor. Stops the protocol handler.
   */
  ~LumenProtocol();

  // Prevent copying and assignment
  LumenProtocol(const LumenProtocol &) = delete;
  LumenProtocol &operator=(const LumenProtocol &) = delete;

  /**
   * @brief Starts the protocol handler and its associated ReliabilityManager.
   * Clears internal buffers.
   */
  void start();

  /**
   * @brief Stops the protocol handler and its associated ReliabilityManager.
   */
  void stop();

  void reset_sequence_number();

  /**
   * @brief Sends a payload through the LUMEN protocol.
   * Adds LUMEN headers, manages sequence numbers, interacts with
   * ReliabilityManager, and sends the resulting packet via the appropriate UDP
   * transport.
   * @param payload The binary payload (typically serialized JSON from
   * MessageManager).
   * @param type The LumenHeader::MessageType indicating the payload type (DATA,
   * CMD, STATUS, etc.).
   * @param priority The LumenHeader::Priority for the message.
   * @param recipient The target endpoint. Required for BASE_STATION mode.
   * Ignored in ROVER mode (sends to registered base).
   */
  void send_message(const std::vector<uint8_t> &payload,
                    LumenHeader::MessageType type,
                    LumenHeader::Priority priority,
                    const udp::endpoint &recipient = udp::endpoint());

  /**
   * @brief Sets the callback function to be invoked when a complete LUMEN
   * packet payload is received and validated. This callback is typically set by
   * the MessageManager to receive payloads for deserialization.
   * @param callback Function taking (payload_bytes, header, sender_endpoint).
   */
  void set_message_callback(
      std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                         const udp::endpoint &)>
          callback);

  /**
   * @brief Gets the next sequence number that will be used for an outgoing
   * packet.
   * @return The current sequence number counter value.
   */
  uint8_t get_current_sequence() const;

  void set_session_active(bool active);

private:
  /**
   * @brief Internal callback handler for raw data received from the UDP layer
   * (UdpServer/UdpClient). Buffers data and attempts to parse complete LUMEN
   * packets or raw JSON.
   * @param data The raw byte vector received.
   * @param endpoint The sender's UDP endpoint.
   */
  void handle_udp_data(const std::vector<uint8_t> &data,
                       const udp::endpoint &endpoint);

  /**
   * @brief Processes a fully parsed and validated LumenPacket.
   * Handles ACK/NAK logic, triggers reliability actions (sending ACKs, checking
   * gaps), and forwards non-control packet payloads to the message_callback_.
   * @param packet The validated LumenPacket.
   * @param endpoint The sender's UDP endpoint.
   */
  void process_complete_packet(const LumenPacket &packet,
                               const udp::endpoint &endpoint);

  /**
   * @brief Sends a pre-constructed LumenPacket over the appropriate UDP
   * transport.
   * @param packet The LumenPacket to send.
   * @param recipient The target UDP endpoint.
   */
  void send_packet(const LumenPacket &packet, const udp::endpoint &recipient);

  /**
   * @brief Sends an ACK packet (Base Station mode only).
   * Acknowledges receipt of a packet with the given sequence number.
   * @param seq_to_ack The sequence number of the packet being acknowledged.
   * @param recipient The endpoint to send the ACK to (the original sender).
   */
  void send_ack(uint8_t seq_to_ack, const udp::endpoint &recipient);

  /**
   * @brief Sends a NAK packet (Rover mode only).
   * Requests retransmission of a packet with the given sequence number.
   * @param seq_requested The sequence number of the missing packet being
   * requested.
   * @param recipient The endpoint to send the NAK to (typically the base
   * station).
   */
  void send_nak(uint8_t seq_requested, const udp::endpoint &recipient);

  /**
   * @brief Generates a 32-bit timestamp (milliseconds since epoch, truncated).
   * @return A 32-bit timestamp.
   */
  uint32_t generate_timestamp() const;

  /**
   * @brief Callback function passed to ReliabilityManager to handle
   * retransmissions.
   * @param packet The LumenPacket that needs to be retransmitted.
   * @param endpoint The target UDP endpoint for retransmission.
   */
  void handle_retransmission(const LumenPacket &packet,
                             const udp::endpoint &endpoint);

  /**
   * @brief Attempts to parse complete LumenPackets from the raw byte buffer
   * associated with an endpoint.
   * @param endpoint_key The string key representing the endpoint.
   * @param endpoint The actual UDP endpoint object.
   */
  void process_frame_buffer(const std::string &endpoint_key,
                            const udp::endpoint &endpoint);

  /**
   * @brief Creates a unique string key from a UDP endpoint address and port.
   * Used for map lookups (e.g., frame_buffers_).
   * @param endpoint The UDP endpoint.
   * @return A string representation (e.g., "192.168.1.100:9001").
   */
  std::string get_endpoint_key(const udp::endpoint &endpoint) const;

  /**
   * @brief Checks for missing sequence numbers based on ReliabilityManager data
   * (Rover mode only). Sends NAKs for detected gaps, respecting debounce logic.
   * @param endpoint The endpoint from which packets are being received
   * (typically the base station).
   */
  void check_sequence_gaps(const udp::endpoint &endpoint);

  // --- Member Variables ---

  ProtocolMode mode_; ///< Current operational mode (BASE_STATION or ROVER).
  UdpServer *server_; ///< Pointer to the UdpServer (used in BASE_STATION mode).
                      ///< Null otherwise.
  UdpClient *client_; ///< Pointer to the UdpClient (used in ROVER mode). Null
                      ///< otherwise.

  ///< Buffer for incoming UDP data, keyed by endpoint string, used for packet
  ///< reassembly.
  std::unordered_map<std::string, std::vector<uint8_t>> frame_buffers_;

  std::atomic<uint8_t> current_sequence_; ///< Atomically incremented sequence
                                          ///< number for outgoing packets.

  ///< Maps endpoint string keys back to actual endpoint objects.
  std::unordered_map<std::string, udp::endpoint> endpoint_map_;

  std::unique_ptr<ReliabilityManager>
      reliability_manager_; ///< Manages reliability logic (ACKs, NAKs,
                            ///< retransmissions).

  ///< Callback function pointer (set by MessageManager) to handle received
  ///< packet payloads.
  std::function<void(const std::vector<uint8_t> &, const LumenHeader &,
                     const udp::endpoint &)>
      message_callback_;

  // Mutexes for protecting shared resources accessed potentially by multiple
  // threads (asio callbacks)
  std::mutex frame_buffers_mutex_; ///< Protects frame_buffers_.
  std::mutex callback_mutex_;      ///< Protects message_callback_.
  std::mutex endpoint_mutex_;      ///< Protects endpoint_map_.

  std::atomic<bool>
      running_; ///< Flag indicating if the protocol handler is active.
  std::atomic<bool> session_active_ = false;

  boost::asio::io_context &io_context_; ///< Reference to the main ASIO context.
};