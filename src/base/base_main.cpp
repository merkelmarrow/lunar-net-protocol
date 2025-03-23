// src/base/base_main.cpp

#include "basic_message.hpp"
#include "udp_server.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ip/address.hpp>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

using boost::asio::ip::udp;

void print_base_usage() {
  std::cout << "Base station v0.1" << std::endl;
  std::cout << "Usage: ./base_station [port]" << std::endl;
  std::cout << "    port: the port to listen on, default is 5000" << std::endl;
  std::cout << "Commands while running:" << std::endl;
  std::cout << "    send <ip> <port> <message>: Send a message to the rover."
            << std::endl;
  std::cout << "    exit: Exit the application." << std::endl;
}

void process_commands(UdpServer &server);

int main() {
  try {
    boost::asio::io_context io_context;
    UdpServer server(io_context, 5000);

    server.set_receive_callback([&server](const std::string &message) {
      std::cout << "[CALLBACK] Received message: " << message << std::endl;
    });

    server.start();
    io_context.run();
  } catch (std::exception &e) {
    std::cerr << "[ERROR] " << e.what() << std::endl;
  }

  return 0;
}

void process_commands(UdpServer &server) {
  std::string line;

  while (std::getline(std::cin, line)) {
    if (line == "exit") {
      std::cout << "[BASE] Shutting down." << std::endl;
      break;
    } else if (line.substr(0, 4) == "send") {
      std::stringstream iss(line.substr(5));
      std::string ip, message_content;
      int port;

      if (iss >> ip >> port) {
        // Read the rest as message content
        std::getline(iss >> std::ws, message_content);

        if (!message_content.empty()) {
          try {
            // create a BasicMessage
            auto message =
                std::make_unique<BasicMessage>(message_content, "base-station");
            std::string serialised = message->serialise();

            // send to the ip address
            boost::asio::ip::udp::endpoint rover_endpoint(
                boost::asio::ip::address::from_string(ip), port);

            server.send_data(serialised, rover_endpoint);

            std::cout << "[BASE] Message sent to " << ip << ":" << port
                      << std::endl;
          } catch (const std::exception &error) {
            std::cerr << "[ERROR] Failed to send message: " << error.what()
                      << std::endl;
          }
        } else {
          std::cerr << "[ERROR] No message content provided." << std::endl;
        }
      } else {
        std::cerr
            << "[ERROR] Invalid command format. Use: send <ip> <port> <message>"
            << std::endl;
      }
    } else if (line == "help") {
      print_base_usage();
    } else if (!line.empty()) {
      std::cerr
          << "[ERROR] Unknown command. Type 'help' for available commands."
          << std::endl;
    }
  }
}
