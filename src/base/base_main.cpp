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
#include <thread>

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

int main(int argc, char *argv[]) {
  try {
    int port = 5000; // default port

    // parse command line arguments
    if (argc > 1) {
      try {
        port = std::stoi(argv[1]);
      } catch (const std::exception &error) {
        std::cerr << "[ERROR] Invalid port number: " << argv[1] << std::endl;
        print_base_usage();
        return 1;
      }
    }

    // Initialize the io_context
    boost::asio::io_context io_context;

    // Create the server
    UdpServer server(io_context, port);

    // Set the callback to process incoming messages
    server.set_receive_callback([&server](
                                    const std::vector<uint8_t> &received_data,
                                    const udp::endpoint &sender) {
      try {
        // Convert binary data to string
        std::string received_str(received_data.begin(), received_data.end());

        // Try to deserialize the message
        if (Message::is_valid_json(received_str)) {
          std::cout << "[BASE] Received JSON message:" << std::endl;
          std::cout << Message::pretty_print(received_str) << std::endl;

          // Deserialize to proper message type
          auto message = Message::deserialise(received_str);

          // Create a response message
          auto response = std::make_unique<BasicMessage>(
              "Message received by base station", "base");

          // Convert response to binary and send
          std::string response_str = response->serialise();
          std::vector<uint8_t> response_data(response_str.begin(),
                                             response_str.end());

          server.send_data(response_data, sender);
        } else {
          std::cout << "[BASE] Received non-JSON message: " << received_str
                    << std::endl;

          // Echo back for non-JSON messages
          std::string response = "Received non-JSON: " + received_str;
          std::vector<uint8_t> response_data(response.begin(), response.end());

          server.send_data(response_data, sender);
        }
      } catch (const std::exception &e) {
        std::cerr << "[ERROR] Failed to process message: " << e.what()
                  << std::endl;
      }
    });

    std::cout << "[BASE] UDP Server starting on port " << port << "."
              << std::endl;
    server.start();

    std::thread io_thread([&io_context]() {
      try {
        io_context.run();
      } catch (const std::exception &error) {
        std::cerr << "[ERROR] IO Context error: " << error.what() << std::endl;
      }
    });

    // Process user commands
    print_base_usage();
    process_commands(server);

    // Clean up
    io_context.stop();
    if (io_thread.joinable()) {
      io_thread.join();
    }

    std::cout << "[BASE] Server stopped." << std::endl;

  } catch (const std::exception &error) {
    std::cerr << "[FATAL ERROR] " << error.what() << std::endl;
    return 1;
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
      std::string ip{}, message_content{};
      int port{};

      if (iss >> ip >> port) {
        // Read the rest as message content
        std::getline(iss >> std::ws, message_content);

        if (!message_content.empty()) {
          try {
            // create a BasicMessage
            auto message =
                std::make_unique<BasicMessage>(message_content, "base-station");
            std::string serialised = message->serialise();

            // Convert to binary data
            std::vector<uint8_t> data(serialised.begin(), serialised.end());

            // create endpoint
            boost::asio::ip::udp::endpoint rover_endpoint(
                boost::asio::ip::address::from_string(ip), port);

            // send to endpoint
            server.send_data(data, rover_endpoint);

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