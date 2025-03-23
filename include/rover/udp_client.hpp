// include/rover/udp_client.hpp

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>

using boost::asio::ip::udp;

class UdpClient {
public:
  UdpClient(boost::asio::io_context &io_context);
  ~UdpClient();
};