// src/rover/udp_client.cpp

#include "udp_client.hpp"
#include <boost/asio/io_context.hpp>

UdpClient::UdpClient(boost::asio::io_context &io_context)
    : io_context_(io_context), socket_(io_context, udp::endpoint(udp::v4(), 0)),
      running_(false) {}