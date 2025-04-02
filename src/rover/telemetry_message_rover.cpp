// src/rover/telemetry_message_rover.cpp

#include "rover.hpp"
#include "telemetry_message.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <thread> // For running io_context and timing

const std::string BASE_HOST = "10.237.0.201";
const int TELEMETRY_PORT = 9002; // Must match the port Base Station is listening on
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

double get_temperature(void)
{
  // uses actual moon high and low temperatures https://science.nasa.gov/moon/facts/
  double temp = -173 + (rand() % 300),
  return temp;
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

    // initialise the random number generator using the time
    srand(time(NULL)); // sets a new seed 

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
    auto last_telemetry_time = std::chrono::steady_clock::now();

    while (!io_context.stopped()) 
    {
      auto current_state = rover.get_session_state();
      auto now = std::chrono::steady_clock::now();
    
      if (current_state == Rover::SessionState::ACTIVE) 
      {
        // Send Telemetry periodically
        if (now - last_telemetry_time > std::chrono::seconds(15)) {
          std::cout << "[ROVER MAIN] Sending Telemetry..." << std::endl;
          std::map<std::string, double> readings = 
          {
              {"temperature", get_temperature()}, 
              {"battery_voltage", 12.1 - (rand() % 5) / 10.0}
            };
          rover.send_telemetry(readings);
          last_telemetry_time = now;
        }

      } 
      else 
      {
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