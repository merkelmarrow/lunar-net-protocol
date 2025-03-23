// src/base/base_main.cpp

#include "udp_server.hpp"
#include <boost/asio.hpp>
#include <iostream>

int main() {
    try {
        boost::asio::io_context io_context;
        UdpServer server(io_context, 5000);

        server.set_receive_callback([&server](const std::string& message) {
            std::cout << "[CALLBACK] Received message: " << message << std::endl;
        });

        server.start();
        io_context.run();
    } catch (std::exception &e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
    }

    return 0;
}
