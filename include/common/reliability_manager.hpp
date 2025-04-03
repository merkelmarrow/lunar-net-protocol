// include/common/reliability_manager.hpp
#pragma once

#include "configs.hpp"      // Include configs for constants
#include "lumen_packet.hpp" // Needs full definition for LumenPacket members
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>    // For storing ACKed sequences
#include <string> // For endpoint keys
#include <unordered_map>
#include <utility> // For std::pair
#include <vector>

// Forward declaration if LumenPacket definition wasn't needed in header members
// class LumenPacket;

using boost::asio::ip::udp;

/**
 * @class ReliabilityManager
 * @brief Manages the reliability aspects of the LUMEN protocol based on role
 * (Base Station or Rover).
 *
 * This class works in conjunction with LumenProtocol to implement ACK/NAK
 * handling, sequence number tracking, timeout-based retransmissions (Rover),
 * and NAK-based retransmissions (Base Station). It uses timers for periodic
 * checks and cleanup.
 *
 * Key Responsibilities:
 * - Tracking Sent Packets: Stores information about packets sent by
 * LumenProtocol that require reliability handling.
 * - Processing ACKs (Rover): Removes successfully acknowledged packets from
 * tracking.
 * - Processing NAKs (Base): Triggers immediate retransmission of NAK'd packets
 * via callback.
 * - Tracking Received Sequences: Records sequence numbers of received packets
 * per endpoint.
 * - Detecting Missing Sequences (Rover): Identifies potential gaps in received
 * sequences to trigger NAK generation in LumenProtocol.
 * - Managing Retransmissions (Rover): Uses a timer to check for timed-out
 * packets and requests their retransmission via callback.
 * - Debouncing NAKs (Rover): Prevents sending NAKs too frequently for the same
 * sequence.
 * - Tracking Sent ACKs (Base): Remembers which sequences have been ACKed to
 * prevent processing duplicate incoming packets.
 * - Periodic Cleanup: Removes old tracking information to prevent unbounded
 * memory usage.
 */
class ReliabilityManager {
public:
  /**
   * @enum Role
   * @brief Defines the operational role, determining the reliability strategy.
   */
  enum class Role {
    BASE_STATION, ///< Sends ACKs for received data, retransmits only upon
                  ///< receiving NAK.
    ROVER         ///< Expects ACKs for sent data, sends NAKs for missing data,
                  ///< retransmits on timeout.
  };

  /**
   * @brief Constructor.
   * @param io_context The Boost.Asio io_context for timers.
   * @param is_base_station True if operating as BASE_STATION, false for ROVER.
   */
  ReliabilityManager(boost::asio::io_context &io_context, bool is_base_station);

  /**
   * @brief Destructor. Stops the manager and cancels timers.
   */
  ~ReliabilityManager();

  // Prevent copying and assignment
  ReliabilityManager(const ReliabilityManager &) = delete;
  ReliabilityManager &operator=(const ReliabilityManager &) = delete;

  /**
   * @brief Starts the manager, clearing state and scheduling timers.
   */
  void start();

  /**
   * @brief Stops the manager and cancels timers.
   */
  void stop();

  /**
   * @brief Adds information about a sent packet for reliability tracking.
   * Called by LumenProtocol for packets that need reliability (not ACKs/NAKs).
   * @param seq The sequence number of the sent packet.
   * @param packet The LumenPacket object that was sent.
   * @param recipient The endpoint the packet was sent to.
   */
  void add_send_packet(uint8_t seq, const LumenPacket &packet,
                       const udp::endpoint &recipient);

  /**
   * @brief Processes a received ACK packet (Rover role only).
   * Removes the corresponding packet from retransmission tracking.
   * @param seq The sequence number of the original packet being acknowledged.
   */
  void process_ack(uint8_t seq);

  /**
   * @brief Processes a received NAK packet (Base Station role only).
   * Triggers immediate retransmission of the requested packet via the
   * retransmit callback.
   * @param seq The sequence number of the packet being requested via NAK.
   */
  void process_nak(uint8_t seq);

  using TimeoutCallback = std::function<void(const udp::endpoint &recipient)>;
  void set_timeout_callback(TimeoutCallback callback);

  /**
   * @brief Records that a packet with a specific sequence number was received
   * from a sender. Used primarily by the Rover to track sequences for gap
   * detection.
   * @param seq The sequence number of the received packet.
   * @param sender The endpoint of the sender.
   */
  void record_received_sequence(uint8_t seq, const udp::endpoint &sender);

  /**
   * @brief Identifies potentially missing sequence numbers based on received
   * history (Rover role only). Checks for gaps within a defined window
   * preceding the highest received sequence number.
   * @param sender The endpoint from which packets are being received.
   * @return A vector of sequence numbers suspected to be missing.
   */
  std::vector<uint8_t> get_missing_sequences(const udp::endpoint &sender);

  /**
   * @brief Checks if a NAK was sent for a specific sequence within the debounce
   * interval (Rover role only).
   * @param seq The sequence number to check.
   * @return True if a NAK was sent recently, false otherwise.
   */
  bool is_recently_naked(uint8_t seq);

  /**
   * @brief Records the time when a NAK was sent for a specific sequence (Rover
   * role only).
   * @param seq The sequence number for which a NAK was sent.
   */
  void record_nak_sent(uint8_t seq);

  /**
   * @brief Gets a list of packets that have timed out and need retransmission
   * (Rover role only). Checks tracked sent packets against their retry counts
   * and calculated timeouts.
   * @return A vector of pairs, each containing the LumenPacket to retransmit
   * and its target endpoint.
   */
  std::vector<std::pair<LumenPacket, udp::endpoint>>
  get_packets_to_retransmit();

  /**
   * @brief Sets the callback function used to trigger packet retransmissions.
   * This callback is invoked by process_nak (Base) or the retransmission timer
   * (Rover). It's typically set by LumenProtocol to point to its own
   * send_packet method.
   * @param callback Function taking (packet_to_retransmit, target_endpoint).
   */
  void set_retransmit_callback(
      std::function<void(const LumenPacket &, const udp::endpoint &)> callback);

  /**
   * @brief Checks if the Base Station has already sent an ACK for a given
   * sequence from an endpoint. Used to prevent processing duplicate incoming
   * packets.
   * @param seq The sequence number to check.
   * @param endpoint The endpoint of the original sender.
   * @return True if an ACK was previously sent, false otherwise.
   */
  bool has_acked_sequence(uint8_t seq, const udp::endpoint &endpoint);

  /**
   * @brief Records that the Base Station has sent an ACK for a given sequence
   * from an endpoint.
   * @param seq The sequence number being ACKed.
   * @param endpoint The endpoint of the original sender.
   */
  void record_acked_sequence(uint8_t seq, const udp::endpoint &endpoint);

  void reset_state();

private:
  /**
   * @struct SentPacketInfo
   * @brief Stores details about a sent packet needed for reliability tracking.
   */
  struct SentPacketInfo {
    LumenPacket packet; ///< The actual packet data.
    std::chrono::steady_clock::time_point
        sent_time;           ///< Time the packet was last sent/retransmitted.
    int retry_count;         ///< Number of retransmission attempts.
    udp::endpoint recipient; ///< The intended recipient endpoint.

    // Constructor for convenience
    SentPacketInfo(const LumenPacket &p,
                   std::chrono::steady_clock::time_point t, int r,
                   const udp::endpoint &e)
        : packet(p), sent_time(t), retry_count(r), recipient(e) {}
  };

  /**
   * @brief Handler for the periodic retransmission timer.
   * Invokes get_packets_to_retransmit (Rover) and triggers the callback if
   * needed. Reschedules the timer.
   */
  void handle_retransmission_timer();

  /**
   * @brief Handler for the periodic cleanup timer.
   * Invokes cleanup_old_entries and reschedules the timer.
   */
  void handle_cleanup_timer();

  /**
   * @brief Removes old entries from tracking maps (received_sequences_,
   * recent_naks_, acked_sequences_).
   */
  void cleanup_old_entries();

  // --- Member Variables ---

  ///< Stores packets sent that are awaiting ACK (Rover) or potential NAK
  ///< (Base). Keyed by sequence number.
  std::map<uint8_t, SentPacketInfo> sent_packets_;

  ///< Stores sequence numbers ACKed by the Base Station, per endpoint key.
  ///< Prevents processing duplicates.
  std::unordered_map<std::string, std::set<uint8_t>> acked_sequences_;
  std::mutex acked_sequences_mutex_; ///< Mutex for acked_sequences_.

  ///< Stores timestamps of received sequences, per endpoint key. Used by Rover
  ///< for gap detection.
  std::unordered_map<std::string,
                     std::map<uint8_t, std::chrono::steady_clock::time_point>>
      received_sequences_;

  ///< Stores timestamps of recently sent NAKs (Rover), keyed by sequence
  ///< number. Used for debounce.
  std::map<uint8_t, std::chrono::steady_clock::time_point> recent_naks_;

  boost::asio::steady_timer
      retransmit_timer_; ///< Timer for periodic retransmission checks (Rover).
  boost::asio::steady_timer
      cleanup_timer_; ///< Timer for periodic cleanup of old tracking data.

  ///< Callback function (set by LumenProtocol) to request packet
  ///< transmission/retransmission.
  std::function<void(const LumenPacket &, const udp::endpoint &)>
      retransmit_callback_;

  // Mutexes for thread safety
  std::mutex sent_packets_mutex_;       ///< Protects sent_packets_.
  std::mutex received_sequences_mutex_; ///< Protects received_sequences_.
  std::mutex recent_naks_mutex_;        ///< Protects recent_naks_.
  std::mutex callback_mutex_;           ///< Protects retransmit_callback_.

  std::atomic<bool> running_; ///< Flag indicating if the manager is active.
  Role role_;                 ///< Operational role (BASE_STATION or ROVER).

  // Timestamp for ACK cleanup (simple periodic full clear)
  std::chrono::steady_clock::time_point last_ack_cleanup_time_{};

  static constexpr uint8_t WINDOW_SIZE = 16;

  TimeoutCallback timeout_callback_ =
      nullptr;                        ///< Callback for max retry notification.
  std::mutex timeout_callback_mutex_; ///< Mutex for timeout_callback_.
  int consecutive_timeouts_to_base_ =
      0; ///< Counter for consecutive timeouts to base (Rover role).
};