// src/base/test_base_main.cpp

#include "base_station.hpp"
#include "basic_message.hpp"
#include "command_message.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <thread> // For running io_context

const int BASE_PORT = 9001;
const std::string BASE_ID = "TestBaseStation";

// --- Callback Handlers ---

// Handles general messages not specifically routed elsewhere
void handle_base_app_message(std::unique_ptr<Message> message,
                             const udp::endpoint &sender) {
  if (!message)
    return;
  std::cout << "[BASE TEST APP] Received message from " << sender
            << ", Type: " << message->get_type() << std::endl;

  if (message->get_type() == BasicMessage::message_type()) {
    auto *basic_msg = dynamic_cast<BasicMessage *>(message.get());
    if (basic_msg) {
      std::cout << "  Basic Content: " << basic_msg->get_content() << std::endl;
    }
  }
  // Add handling for other expected application-specific message types here
}

// Handles Status and Telemetry messages
void handle_base_status_telemetry(const std::string &rover_id,
                                  const std::map<std::string, double> &data) {
  std::cout << "[BASE TEST STATUS/TELEMETRY] Received data from Rover ID: "
            << rover_id << std::endl;

  // Check if it contains status_level (indicating a StatusMessage)
  if (data.count("status_level")) {
    int level_int = static_cast<int>(data.at("status_level"));
    std::string level_str = "UNKNOWN";
    // Note: Cannot get description easily here as map stores doubles
    switch (static_cast<StatusMessage::StatusLevel>(level_int)) {
    case StatusMessage::StatusLevel::OK:
      level_str = "OK";
      break;
    case StatusMessage::StatusLevel::WARNING:
      level_str = "WARNING";
      break;
    case StatusMessage::StatusLevel::ERROR:
      level_str = "ERROR";
      break;
    case StatusMessage::StatusLevel::CRITICAL:
      level_str = "CRITICAL";
      break;
    }
    std::cout << "  Status Level: " << level_str << " (" << level_int << ")"
              << std::endl;
  } else {
    // Assume TelemetryMessage
    std::cout << "  Telemetry Readings:" << std::endl;
    for (const auto &pair : data) {
      std::cout << "    " << pair.first << ": " << pair.second << std::endl;
    }
  }
}

int main() {
  try {
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

    // Create Base Station
    BaseStation base_station(io_context, BASE_PORT, BASE_ID);

    // Register callbacks
    base_station.set_application_message_handler(handle_base_app_message);
    base_station.set_status_callback(handle_base_status_telemetry);

    // Start the base station
    base_station.start();
    std::cout << "[BASE MAIN] Base Station started on port " << BASE_PORT
              << ". Waiting for Rover..." << std::endl;

    // Run io_context in a separate thread
    std::thread io_thread([&io_context]() {
      try {
        io_context.run();
      } catch (const std::exception &e) {
        std::cerr << "Exception in io_context thread: " << e.what()
                  << std::endl;
      }
      std::cout << "[BASE MAIN] io_context finished." << std::endl;
    });

    // --- Example Interaction Logic (Runs after setup) ---
    bool command_sent = false;
    while (!io_context.stopped()) {
      // Wait for the session to become active
      if (base_station.get_session_state() ==
          BaseStation::SessionState::ACTIVE) {
        if (!command_sent) {
          std::cout << "[BASE MAIN] Session Active! Sending test command..."
                    << std::endl;
          // Send a command message once connected
          base_station.send_command("TEST_COMMAND",
                                    "param1=value1,param2=value2");
          command_sent = true; // Send only once
        }
      } else {
        // Reset flag if session becomes inactive again
        command_sent = false;
      }

      // Prevent busy-waiting
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // --- Shutdown ---
    std::cout << "[BASE MAIN] Shutting down Base Station..." << std::endl;
    base_station.stop(); // Stop the application logic first

    // io_context might already be stopped by signal handler,
    // but ensure run() exits if it hasn't already.
    if (!io_context.stopped()) {
      io_context.stop();
    }

    if (io_thread.joinable()) {
      io_thread.join(); // Wait for the io_context thread to finish
    }

    std::cout << "[BASE MAIN] Base Station stopped cleanly." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[BASE MAIN] Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}