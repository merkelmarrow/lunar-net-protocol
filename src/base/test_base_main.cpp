// src/base/test_base_main.cpp

#include "base_station.hpp"
#include "message.hpp"
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

std::atomic<bool> shutdown_requested = false;

// simple command line input thread for testing base station commands
void command_input_thread(BaseStation &base,
                          boost::asio::io_context &io_context) {
  std::string line;
  std::cout << "[CMD INPUT] Enter commands (e.g., 'low_power on', 'set_target "
               "53.1 -6.5', 'quit'):"
            << std::endl;
  while (std::getline(std::cin, line)) {
    if (shutdown_requested)
      break; // exit if shutdown requested

    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
      tokens.push_back(token);
    }

    if (tokens.empty())
      continue;

    std::string command = tokens[0];

    if (command == "quit") {
      std::cout << "[CMD INPUT] Quit command received. Signaling shutdown..."
                << std::endl;
      shutdown_requested = true;
      // post a task to safely stop base station and io_context
      boost::asio::post(io_context, [&]() {
        base.stop();
        io_context.stop();
      });
      break; // exit input loop
    } else if (command == "low_power" && tokens.size() == 2) {
      bool enable = (tokens[1] == "on");
      std::cout << "[CMD INPUT] Requesting low power mode: "
                << (enable ? "ON" : "OFF") << std::endl;
      // post action to io_context to run on main thread
      boost::asio::post(io_context,
                        [&base, enable]() { base.set_low_power_mode(enable); });
    } else if (command == "set_target" && tokens.size() == 3) {
      try {
        double lat = std::stod(tokens[1]);
        double lon = std::stod(tokens[2]);
        std::cout << "[CMD INPUT] Setting target coordinates: Lat=" << lat
                  << ", Lon=" << lon << std::endl;
        // post action to io_context
        boost::asio::post(io_context, [&base, lat, lon]() {
          base.set_rover_target(lat, lon);
        });
      } catch (const std::exception &e) {
        std::cerr << "[CMD INPUT] Invalid coordinates: " << e.what()
                  << std::endl;
      }
    } else {
      std::cerr << "[CMD INPUT] Unknown command or incorrect arguments: "
                << line << std::endl;
    }
    if (shutdown_requested)
      break; // re-check after processing
  }
  std::cout << "[CMD INPUT] Input thread finished." << std::endl;
}

int main() {
  const int LISTEN_PORT = 9000; // port for the base station to listen on
  const std::string STATION_ID = "grp18-base";

  try {
    boost::asio::io_context io_context;

    // handle sigint/sigterm for graceful shutdown
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code & /*error*/,
                           int /*signal_number*/) {
      std::cout << "[BASE MAIN] Signal received. Signaling shutdown..."
                << std::endl;
      shutdown_requested = true;
      // post stop actions to ensure thread safety
      boost::asio::post(io_context, [&]() {
        io_context.stop(); // stop the event loop
      });
    });

    // create the basestation instance
    BaseStation base(io_context, LISTEN_PORT, STATION_ID);

    // set a simple handler to print received messages
    base.set_application_message_handler(
        [&](std::unique_ptr<Message> message,
            const boost::asio::ip::udp::endpoint &sender) {
          if (shutdown_requested || !message)
            return;
          std::cout << "[BASE MAIN] Received message type '"
                    << message->get_type() << "' from "
                    << sender.address().to_string() << ":" << sender.port()
                    << std::endl;
          try {
            std::cout << "  Content:\n"
                      << Message::pretty_print(message->serialise())
                      << std::endl;
          } catch (const std::exception &e) {
            std::cerr << "  Error pretty-printing message: " << e.what()
                      << std::endl;
          }
        });

    // start the base station
    base.start();

    std::cout << "[BASE MAIN] Base station started on port " << LISTEN_PORT
              << "." << std::endl;
    std::cout << "[BASE MAIN] Waiting for connections..." << std::endl;

    // start command input thread
    std::thread input_thread(command_input_thread, std::ref(base),
                             std::ref(io_context));

    // run the asio event loop
    io_context.run();

    std::cout << "[BASE MAIN] io_context stopped." << std::endl;

    // wait for input thread to finish
    if (input_thread.joinable()) {
      // potentially send newline to stdin to unblock getline if needed
      input_thread.join();
    }
    std::cout << "[BASE MAIN] Input thread joined." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[BASE MAIN] Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}