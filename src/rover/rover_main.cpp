// src/rover/rover_main.cpp

#include "basic_message.hpp"
#include "message.hpp"
#include "udp_client.hpp"
#include <boost/asio.hpp>
#include <iostream>
#include <string>
#include <thread>

void print_usage() {
  std::cout << "Rover v0.1" << std::endl;
  std::cout << "Usage: ./rover <base_ip> <base_port>" << std::endl;
  std::cout << "Commands while running:" << std::endl;
  std::cout << "  send <message>: Send a message to the base station"
            << std::endl;
  std::cout << "  exit: Exit the application" << std::endl;
}

void process_commands(UdpClient &rover);

int main(int argc, char *argv[]) {
  try {
    // check command line arguments
    if (argc < 3) {
      std::cerr << "[ERROR] Missing required arguments" << std::endl;
      print_usage();
      return 1;
    }

    std::string base_ip = argv[1];
    int base_port;

    try {
      base_port = std::stoi(argv[2]);
    } catch (const std::exception &e) {
      std::cerr << "[ERROR] Invalid port number: " << argv[2] << std::endl;
      print_usage();
      return 1;
    }

    // initialize the io_context
    boost::asio::io_context io_context;

    // create the client
    UdpClient rover(io_context);

    // register with the base station
    try {
      rover.register_base(base_ip, base_port);
      std::cout << "[ROVER] Registered base station endpoint at " << base_ip
                << ":" << base_port << std::endl;
    } catch (const std::exception &e) {
      std::cerr << "[ERROR] Failed to register base station endpoint: "
                << e.what() << std::endl;
      return 1;
    }

    // set the callback to process incoming messages
    rover.set_receive_callback([](const std::string &received_data) {
      try {
        // try to deserialize the message
        if (Message::is_valid_json(received_data)) {
          std::cout << "[ROVER] Received JSON message:" << std::endl;
          std::cout << Message::pretty_print(received_data) << std::endl;

          // deserialize to proper message type
          auto message = Message::deserialise(received_data);

          // print message details
          if (auto basic_msg = dynamic_cast<BasicMessage *>(message.get())) {
            std::cout << "[ROVER] Message content: " << basic_msg->get_content()
                      << std::endl;
            std::cout << "[ROVER] Sender: " << basic_msg->get_sender()
                      << std::endl;
          }
        } else {
          std::cout << "[ROVER] Received non-JSON message: " << received_data
                    << std::endl;
        }
      } catch (const std::exception &e) {
        std::cerr << "[ERROR] Failed to process message: " << e.what()
                  << std::endl;
      }
    });

    // start receiving data
    rover.start_receive();

    // run the io_context in a separate thread
    std::thread io_thread([&io_context]() {
      try {
        io_context.run();
      } catch (const std::exception &e) {
        std::cerr << "[ERROR] IO context error: " << e.what() << std::endl;
      }
    });

    // process user commands
    print_usage();
    process_commands(rover);

    // clean up
    io_context.stop();
    if (io_thread.joinable()) {
      io_thread.join();
    }

    std::cout << "[ROVER] Client stopped." << std::endl;
  } catch (const std::exception &error) {
    std::cerr << "[FATAL ERROR] " << error.what() << std::endl;
    return 1;
  }

  return 0;
}

// function to handle user input commands
void process_commands(UdpClient &rover) {
  std::string line;

  while (std::getline(std::cin, line)) {
    if (line == "exit") {
      std::cout << "[ROVER] Shutting down..." << std::endl;
      break;
    } else if (line.substr(0, 4) == "send") {
      // parse send command: send <message>
      std::string message_content = line.substr(5);

      if (!message_content.empty()) {
        try {
          // create a BasicMessage
          auto message =
              std::make_unique<BasicMessage>(message_content, "rover");
          std::string serialized = message->serialise();

          // send to the base station
          rover.send_data(serialized);

          std::cout << "[ROVER] Message sent to base station" << std::endl;
        } catch (const std::exception &error) {
          std::cerr << "[ERROR] Failed to send message: " << error.what()
                    << std::endl;
        }
      } else {
        std::cerr << "[ERROR] No message content provided" << std::endl;
      }
    } else if (line == "help") {
      print_usage();
    } else if (!line.empty()) {
      std::cerr
          << "[ERROR] Unknown command. Type 'help' for available commands."
          << std::endl;
    }
  }
}