// src/rover/status_message_rover

#include "rover.hpp"
#include "status_message.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <thread> // For running io_context and timing

const std::string BASE_HOST = "10.237.0.201";
const int STATUS_PORT = 9003; // Must match the port Base Station is listening on
const std::string ROVER_ID = "TestRover01";

// A helper function to parse command parameters from a string.
std::vector<std::string> parseParameters(const std::string &paramStr) {
  std::vector<std::string> params;
  std::istringstream iss(paramStr);
  std::string token;
  while (iss >> token) {
      params.push_back(token);
  }
  return params;
}

// --- Callback Handlers ---

// Handles general messages (like commands) received from the Base Station
void handle_rover_app_message(std::unique_ptr<Message> message,
                              const udp::endpoint &sender) {
  if (!message)
    return;
  std::cout << "[ROVER TEST APP] Received message from " << sender
            << ", Type: " << message->get_type() << std::endl;

  if (message->get_type() == CommandMessage::message_type()) {
    auto *cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    if (cmd_msg) {
      std::cout << "  Command: " << cmd_msg->get_command() << std::endl;
      std::cout << "  Params:  " << cmd_msg->get_params() << std::endl;
      // Add logic here to act on the command

      // Parse parameters if needed
      std::vector<std::string> paramList = parseParameters(params);

      if (command == "MOVE_FORWARD") 
      {
        if (!paramList.empty()) {
          int distance = std::stoi(paramList[0]);
          std::cout << "[ROVER APP] Executing MOVE_FORWARD for "
                    << distance << " units." << std::endl;
          //rover movement logic here.
        } 
        else 
        {
          std::cout << "[ROVER APP] MOVE_FORWARD command missing distance parameter."
                    << std::endl;
        }
      }
      else if (command == "STOP") 
      {
        std::cout << "[ROVER APP] Executing STOP command." << std::endl;
        // logic to stop the rover.
      }
      else if (command == "COLLECT_SAMPLE") 
      {
        std::cout << "[ROVER APP] Executing COLLECT_SAMPLE command." << std::endl;
        // logic to simulate sample collection.
      }
    }
  }
  else {
    // Handle other message types (Telemetry, Status, Basic, etc.)
    std::cout << "[ROVER APP] Received non-command message of type: "
              << message->get_type() << std::endl;
  }
}

int main() 
{
  try 
  {
    boost::asio::io_context io_context;

    // Set up signal handling for graceful shutdown
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code &error, int signal_number) {
          if (!error) {
            std::cout << "Signal " << signal_number << " received. Stopping..."
                      << std::endl;
            io_context.stop(); // Request io_context to stop
          }
        });

    // Create Rover
    Rover rover(io_context, BASE_HOST, BASE_PORT, ROVER_ID);

    // Register callback
    rover.set_application_message_handler(handle_rover_app_message);

    // Start the rover (initiates handshake)
    rover.start();
    std::cout << "[ROVER MAIN] Rover started. Attempting handshake with "
              << BASE_HOST << ":" << BASE_PORT << std::endl;

    // Run io_context in a separate thread
    std::thread io_thread([&io_context]() {
      try {
        io_context.run();
      } catch (const std::exception &e) {
        std::cerr << "Exception in io_context thread: " << e.what()
                  << std::endl;
      }
      std::cout << "[ROVER MAIN] io_context finished." << std::endl;
    });

    // --- Example Interaction Logic (Runs after setup) ---
    int status_update_counter = 0;

    while (!io_context.stopped()) 
    {
      auto current_state = rover.get_session_state();
      auto now = std::chrono::steady_clock::now();
    
      if (current_state == Rover::SessionState::ACTIVE) 
      {
        // Update Status periodically (sent automatically by internal timer)
        if (status_update_counter % 20 == 0) // Update roughly every 20s
        { 
        std::cout << "[ROVER MAIN] Updating internal status..." << std::endl;
        rover.update_status(StatusMessage::StatusLevel::OK,
                            "All systems nominal.");
        } 
        else if (status_update_counter % 20 == 10) 
        {
        rover.update_status(StatusMessage::StatusLevel::WARNING,
                            "Battery getting low.");
        }
        status_update_counter++;
      } 
      else 
      {
        // Reset flag if session becomes inactive
        basic_message_sent = false;
        if (current_state == Rover::SessionState::INACTIVE) {
          std::cout << "[ROVER MAIN] Session Inactive. Waiting or retrying "
                       "handshake..."
                    << std::endl;
          // Optional: Add logic to explicitly restart handshake if needed
          // rover.start(); // Careful: could create loops if connection fails
          // repeatedly
        } 
        else 
        {
          std::cout << "[ROVER MAIN] Session state: "
                    << static_cast<int>(current_state) << " (Handshaking)"
                    << std::endl;
        }
      }

      // Prevent busy-waiting
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // --- Shutdown ---
    std::cout << "[ROVER MAIN] Shutting down Rover..." << std::endl;
    rover.stop(); // Stop the application logic first

    // io_context might already be stopped by signal handler,
    // but ensure run() exits if it hasn't already.
    if (!io_context.stopped()) {
      io_context.stop();
    }

    if (io_thread.joinable()) {
      io_thread.join(); // Wait for the io_context thread to finish
    }

    std::cout << "[ROVER MAIN] Rover stopped cleanly." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[ROVER MAIN] Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}