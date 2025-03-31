#pragma once

#include "lumen_protocol.hpp"
#include "message.hpp"
#include "udp_server.hpp"
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>

#include "message_manager.hpp"

#include <map>

using boost::asio::ip::udp;

class BaseStation {
public:
  enum class SessionState {
    INACTIVE,
    HANDSHAKE_INIT,
    HANDSHAKE_ACCEPT,
    ACTIVE
  };

  BaseStation(boost::asio::io_context &io_context, int port,
              const std::string &station_id);
  ~BaseStation();

  void start();
  void stop();

  using StatusCallback = std::function<void(
      const std::string &, const std::map<std::string, double> &)>;
  void set_status_callback(StatusCallback callback);

  SessionState get_session_state() const;
  std::string get_connected_rover_id() const;

  void send_raw_message(const Message &message, const udp::endpoint &endpoint);
  void send_command(const std::string &command, const std::string &params);

private:
  void handle_message(std::unique_ptr<Message> message,
                      const udp::endpoint &sender);

  void handle_session_init(const std::string &rover_id,
                           const udp::endpoint &sender);
  void handle_session_confirm(const std::string &rover_id,
                              const udp::endpoint &sender);

  boost::asio::io_context &io_context_;
  std::unique_ptr<UdpServer> server_;
  std::unique_ptr<LumenProtocol> protocol_;
  std::unique_ptr<MessageManager> message_manager_;

  // session state
  SessionState session_state_;
  std::string connected_rover_id_;
  udp::endpoint rover_endpoint_;

  // base station id
  std::string station_id_;

  // callbacks
  StatusCallback status_callback_;

  // thread safety
  mutable std::mutex state_mutex_;
  std::mutex callback_mutex_;
};