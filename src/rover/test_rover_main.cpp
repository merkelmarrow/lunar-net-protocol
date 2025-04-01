#include "basic_message.hpp" // Needed for raw message sending
#include "command_message.hpp"
#include "rover.hpp"
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <iostream>
#include <sstream> // For parsing input
#include <string>
#include <thread>

// Simple application message handler
void handle_incoming_message(std::unique_ptr<Message> message,
                             const udp::endpoint &sender) {
  if (!message)
    return;

  std::cout << "\n<-- [APP] Received Message from " << sender << " <--\n";
  std::cout << "  Type:    " << message->get_type() << "\n";
  std::cout << "  Sender:  " << message->get_sender() << "\n";
  std::cout << "  Timestamp: "
            << tp_utils::tp_to_string(message->get_timestamp()) << "\n";

  if (message->get_type() == CommandMessage::message_type()) {
    auto cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    if (cmd_msg) {
      std::cout << "  Command: " << cmd_msg->get_command() << "\n";
      std::cout << "  Params:  " << cmd_msg->get_params() << "\n";
    }
  } else if (message->get_type() == BasicMessage::message_type()) {
    auto basic_msg = dynamic_cast<BasicMessage *>(message.get());
    if (basic_msg) {
      std::cout << "  Content: " << basic_msg->get_content() << "\n";
    }
  } else {
    std::cout << "  (Raw JSON): " << message->serialise() << "\n";
  }
  std::cout << "<-- [APP] End Message <--\n" << std::endl;
  std::cout << "> Enter command (cmd <name> [params] | raw <json> | status "
               "<level> <desc> | quit): "
            << std::flush;
}

// Function to run in the input thread
void input_loop(Rover &rover, boost::asio::io_context &io_context) {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "quit") {
      std::cout << "[Input] Quit command received. Stopping..." << std::endl;
      rover.stop();
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
      // Trim leading space from params if it exists
      if (!params.empty() && params[0] == ' ') {
        params = params.substr(1);
      }

      if (rover.get_session_state() == Rover::SessionState::ACTIVE) {
        std::cout << "[Input] Sending Command: " << command_name
                  << " Params: " << params << std::endl;
        rover.send_command(command_name, params);
      } else {
        std::cout << "[Input] Cannot send command, session not active."
                  << std::endl;
      }

    } else if (type == "raw") {
      std::string json_content;
      std::getline(ss, json_content); // Get the rest as JSON content
                                      // Trim leading space
      if (!json_content.empty() && json_content[0] == ' ') {
        json_content = json_content.substr(1);
      }

      if (Message::is_valid_json(json_content)) {
        if (rover.get_session_state() == Rover::SessionState::ACTIVE) {
          try {
            // We need sender_id for BasicMessage, get it from rover if possible
            // This assumes you might add a getter for rover_id_ in Rover class
            // Or hardcode it here for simplicity
            std::string rover_id = "grp18-rover-app"; // Hardcoded for example
            BasicMessage raw_msg(
                json_content,
                rover_id); // Assuming content IS the raw JSON for this example

            // Raw messages often don't use the standard 'send_message' via
            // protocol They might be sent directly via UDP. Let's use
            // send_raw_message
            udp::endpoint base_endpoint =
                rover.get_base_endpoint(); // Need getter in Rover
            std::cout << "[Input] Sending Raw JSON: " << json_content
                      << std::endl;
            rover.send_raw_message(raw_msg, base_endpoint);

          } catch (const std::exception &e) {
            std::cerr << "[Input] Error creating/sending raw message: "
                      << e.what() << std::endl;
          }
        } else {
          std::cout << "[Input] Cannot send raw message, session not active."
                    << std::endl;
        }
      } else {
        std::cout << "[Input] Invalid JSON provided for raw message."
                  << std::endl;
      }
    } else if (type == "status") {
      int level_int;
      std::string description;
      ss >> level_int;
      std::getline(ss, description);
      if (!description.empty() && description[0] == ' ') {
        description = description.substr(1);
      }

      StatusMessage::StatusLevel level =
          StatusMessage::StatusLevel::OK; // Default
      if (level_int >= 0 && level_int <= 3) {
        level = static_cast<StatusMessage::StatusLevel>(level_int);
      } else {
        std::cout
            << "[Input] Invalid status level (0=OK, 1=WARN, 2=ERR, 3=CRIT)."
            << std::endl;
        continue;
      }
      std::cout << "[Input] Updating status to Level " << level_int << ": "
                << description << std::endl;
      rover.update_status(level, description);
      // Status is sent periodically by rover's internal timer, no need to force
      // send usually rover.send_status(); // Uncomment to force immediate send

    } else if (!line.empty()) {
      std::cout << "[Input] Unknown command type: '" << type
                << "'. Use 'cmd', 'raw', 'status', or 'quit'." << std::endl;
    }
    std::cout << "> Enter command (cmd <name> [params] | raw <json> | status "
                 "<level> <desc> | quit): "
              << std::flush;
  }
  std::cout << "[Input] Input loop finished." << std::endl;
}

int main() {
  try {
    boost::asio::io_context io_context;
    // --- Configuration ---
    std::string base_ip = "127.0.0.1"; // Use localhost IP
    int base_port = 9000;
    std::string rover_id = "grp18-rover-cli"; // Unique ID

    std::cout << "[MAIN] Creating Rover..." << std::endl;
    Rover rover(io_context, base_ip, base_port, rover_id);

    // --- Setup Application Callback ---
    rover.set_application_message_handler(handle_incoming_message);
    std::cout << "[MAIN] Application message handler set." << std::endl;

    // --- Start the Rover (initiates handshake) ---
    rover.start();
    std::cout << "[MAIN] Rover started. Attempting connection to " << base_ip
              << ":" << base_port << std::endl;

    // --- Graceful Shutdown Handling ---
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code &error, int signal_number) {
          if (!error) {
            std::cout << "\n[MAIN] Signal " << signal_number
                      << " received. Shutting down..." << std::endl;
            // We stop the io_context from the input thread or signal handler
            // The input thread needs to detect this stop.
            io_context.stop();
            rover.stop(); // Ensure rover components are stopped
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
        io_context.stop(); // Ensure stop on exception
        rover.stop();
      }
    });

    // --- Start input thread ---
    std::cout << "[MAIN] Starting input thread..." << std::endl;
    std::thread input_thread(input_loop, std::ref(rover), std::ref(io_context));

    // --- Wait for threads ---
    std::cout << "[MAIN] Waiting for threads to complete..." << std::endl;
    std::cout << "> Enter command (cmd <name> [params] | raw <json> | status "
                 "<level> <desc> | quit): "
              << std::flush;

    // Wait for the input thread to finish (it finishes when "quit" is entered
    // or io_context stops)
    if (input_thread.joinable()) {
      input_thread.join();
    }
    std::cout << "[MAIN] Input thread joined." << std::endl;

    // Wait for the network thread to finish
    if (io_thread.joinable()) {
      io_thread.join();
    }
    std::cout << "[MAIN] Network thread joined." << std::endl;
    std::cout << "[MAIN] Rover finished." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Exception in main: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}