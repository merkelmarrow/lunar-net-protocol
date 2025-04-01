#include "base_station.hpp"
#include "basic_message.hpp"
#include "command_message.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

// Simple handler for Status/Telemetry (optional, can be merged)
void handle_rover_updates(const std::string &rover_id,
                          const std::map<std::string, double> &data) {
  std::cout << "\n<-- [STATUS/TELEMETRY] From " << rover_id << " <--\n  Data: ";
  for (const auto &entry : data) {
    std::cout << entry.first << "=" << entry.second << "; ";
  }
  std::cout << "\n<-- End Update <--\n" << std::endl;
  std::cout << "> Enter command (cmd <name> [params] | quit): " << std::flush;
}

// Simple generic handler for other messages
void handle_generic_message(std::unique_ptr<Message> message,
                            const udp::endpoint &sender) {
  if (!message)
    return;

  std::cout << "\n<-- [APP] Received Message from " << sender << " <--\n";
  std::cout << "  Type:    " << message->get_type() << "\n";
  std::cout << "  Sender:  " << message->get_sender() << "\n";
  std::cout << "  Timestamp: "
            << tp_utils::tp_to_string(message->get_timestamp()) << "\n";

  if (message->get_type() == BasicMessage::message_type()) {
    auto basic_msg = dynamic_cast<BasicMessage *>(message.get());
    if (basic_msg) {
      std::cout << "  Content: " << basic_msg->get_content() << "\n";
    }
  } else if (message->get_type() == CommandMessage::message_type()) {
    // Might receive commands from Rover (e.g., acknowledgements)
    auto cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    if (cmd_msg) {
      std::cout << "  Command: " << cmd_msg->get_command() << "\n";
      std::cout << "  Params:  " << cmd_msg->get_params() << "\n";
    }
  }
  // Avoid logging Status/Telemetry as 'unhandled' if specific handler exists
  else if (message->get_type() != StatusMessage::message_type() &&
           message->get_type() != TelemetryMessage::message_type()) {
    std::cout << "  (Raw JSON): " << message->serialise() << "\n";
  }

  std::cout << "<-- [APP] End Message <--\n" << std::endl;
  std::cout << "> Enter command (cmd <name> [params] | quit): " << std::flush;
}

// Function to run in the input thread
void input_loop(BaseStation &base_station,
                boost::asio::io_context &io_context) {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "quit") {
      std::cout << "[Input] Quit command received. Stopping..." << std::endl;
      base_station.stop();
      io_context.stop(); // Stop the network thread
      break;
    }

    std::stringstream ss(line);
    std::string type;
    ss >> type;

    if (type == "cmd") {
      std::string command_name;
      std::string params;
      ss >> command_name;       // Get command name
      std::getline(ss, params); // Get the rest as parameters
                                // Trim leading space
      if (!params.empty() && params[0] == ' ') {
        params = params.substr(1);
      }

      if (base_station.get_session_state() ==
          BaseStation::SessionState::ACTIVE) {
        std::cout << "[Input] Sending Command to "
                  << base_station.get_connected_rover_id() << ": "
                  << command_name << " Params: " << params << std::endl;
        base_station.send_command(command_name, params);
      } else {
        std::cout << "[Input] Cannot send command, no active rover session."
                  << std::endl;
      }
    } else if (!line.empty()) {
      std::cout << "[Input] Unknown command type: '" << type
                << "'. Use 'cmd' or 'quit'." << std::endl;
    }
    std::cout << "> Enter command (cmd <name> [params] | quit): " << std::flush;
  }
  std::cout << "[Input] Input loop finished." << std::endl;
}

int main() {
  try {
    boost::asio::io_context io_context;
    int port = 9000; // Port for the base station
    std::string station_id = "BaseStation-CLI";

    std::cout << "[MAIN] Creating BaseStation..." << std::endl;
    BaseStation base_station(io_context, port, station_id);

    // --- Setup Application Callbacks ---
    base_station.set_status_callback(
        handle_rover_updates); // Optional specific handler
    base_station.set_application_message_handler(
        handle_generic_message); // Generic handler

    // --- Start the Base Station ---
    base_station.start();
    std::cout << "[MAIN] Base Station started on port " << port
              << ". Waiting for rover connection..." << std::endl;

    // --- Graceful Shutdown Handling ---
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code &error, int signal_number) {
          if (!error) {
            std::cout << "\n[MAIN] Signal " << signal_number
                      << " received. Shutting down..." << std::endl;
            io_context.stop();
            base_station.stop();
          }
        });

    // --- Run io_context in a separate thread ---
    std::thread io_thread([&]() {
      try {
        std::cout << "[MAIN] Network thread (io_context) started." << std::endl;
        io_context.run();
        std::cout << "[MAIN] Network thread (io_context) finished."
                  << std::endl;
      } catch (const std::exception &e) {
        std::cerr << "[ERROR] Exception in network thread: " << e.what()
                  << std::endl;
        io_context.stop();
        base_station.stop();
      }
    });

    // --- Start input thread ---
    std::cout << "[MAIN] Starting input thread..." << std::endl;
    std::thread input_thread(input_loop, std::ref(base_station),
                             std::ref(io_context));

    // --- Wait for threads ---
    std::cout << "[MAIN] Waiting for threads to complete..." << std::endl;
    std::cout << "> Enter command (cmd <name> [params] | quit): " << std::flush;

    if (input_thread.joinable()) {
      input_thread.join();
    }
    std::cout << "[MAIN] Input thread joined." << std::endl;

    if (io_thread.joinable()) {
      io_thread.join();
    }
    std::cout << "[MAIN] Network thread joined." << std::endl;
    std::cout << "[MAIN] Base Station finished." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Exception in main: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}