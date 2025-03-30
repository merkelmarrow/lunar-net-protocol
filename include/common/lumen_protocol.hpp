#pragma once

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <vector>

#include "../base/udp_server.hpp"
#include "../rover/udp_client.hpp"

using boost::asio::ip::udp;

class LumenProtocol {
public:
  // constructor for base station
  LumenProtocol(boost::asio::io_context &io_context, UdpServer &server);

  // constructor for rover
  LumenProtocol(boost::asio::io_context &io_context, UdpClient &client);

  // destructor
  ~LumenProtocol();

  void start();
  void stop();

  void send_message(const std::vector<uint8_t> &payload,
                    const udp::endpoint &recipient = udp::endpoint());

private:
  enum class Mode { SERVER, CLIENT };
};