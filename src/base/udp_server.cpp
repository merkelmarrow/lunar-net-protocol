// src/base/udp_server.cpp

#include "udp_server.hpp"
#include <iostream>

UdpServer::UdpServer(boost::asio::io_context& context, int port)
    : socket_(context, udp::endpoint(udp::v4(), port)) {}

void UdpServer::start() {
    std::cout << "[SERVER] UDP Server is running..." << std::endl;
    receive_data();
}

void UdpServer::set_receive_callback(std::function<void(const std::string&)> callback) {
    receive_callback_ = std::move(callback);
}

void UdpServer::send_data(const std::string& message, const udp::endpoint& recipient) {
    socket_.async_send_to(
        boost::asio::buffer(message), recipient,
        [this](boost::system::error_code ec, std::size_t /*length*/) {
            if (ec) {
                std::cerr << "[ERROR] Failed to send data: " << ec.message() << std::endl;
            }
        });
}

void UdpServer::receive_data() {
    socket_.async_receive_from(
        boost::asio::buffer(buffer_), sender_endpoint_,
        [this](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                std::string received_message(buffer_, length);
                std::cout << "[RECEIVED] Message: " << received_message << std::endl;

                if (receive_callback_) {
                    receive_callback_(received_message);
                }

                // Example response message (Echoing back)
                send_data("Received: " + received_message, sender_endpoint_);
            }
            receive_data();  // Continue listening
        });
}
