#ifndef UDP_SERVER_HPP
#define UDP_SERVER_HPP

#include <iostream>
#include <boost/asio.hpp>
#include <functional>

using boost::asio::ip::udp;

class UdpServer {
public:
    UdpServer(boost::asio::io_context& context, int port);
    void start();
    void set_receive_callback(std::function<void(const std::string&)> callback);
    void send_data(const std::string& message, const udp::endpoint& recipient);

private:
    void receive_data();

    udp::socket socket_;
    udp::endpoint sender_endpoint_;
    char buffer_[1024];
    std::function<void(const std::string&)> receive_callback_;
};

#endif  // UDP_SERVER_HPP
