// src/base/test_base_main.cpp
#include "base_station.hpp"
#include "basic_message.hpp" // To send/receive basic messages
#include "command_message.hpp"
#include <boost/asio.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread> // For std::this_thread

const int LISTEN_PORT = 9001;                       // Port the base listens on
const std::string BASE_STATION_ID = "test-base-01"; // ID for the base station

// Forward declaration for the base station instance to use in the handler
BaseStation *g_base_station = nullptr;

// Custom application message handler for the Base Station
void handle_base_message(std::unique_ptr<Message> message,
                         const udp::endpoint &sender) {
  if (!message) {
    std::cout << "[BASE APP] Received null message pointer." << std::endl;
    return;
  }
  std::cout << "[BASE APP] Received message type: " << message->get_type()
            << " from sender ID: " << message->get_sender()
            << " at endpoint: " << sender << std::endl;

  // Example: Handle a BasicMessage received from the rover
  if (message->get_type() == BasicMessage::message_type()) {
    auto basic_msg = dynamic_cast<BasicMessage *>(message.get());
    if (basic_msg) {
      std::cout << "[BASE APP] Received BasicMessage content: "
                << basic_msg->get_content() << std::endl;

      // Send a response back to the specific rover that sent the message
      if (g_base_station) {
        std::cout << "[BASE APP] Sending BasicMessage response back to "
                  << sender << std::endl;
        BasicMessage response_msg("Hello back from Base!", BASE_STATION_ID);
        // Use send_raw_message to target the specific endpoint
        // Or modify send_command if appropriate, but send_raw_message might be
        // simpler for basic replies directly to the source endpoint
        // Alternatively, ensure send_message targets the correct rover endpoint
        // stored during handshake. Using send_message via MessageManager is
        // preferred.
        g_base_station->send_message(response_msg, sender);
      }
    }
  } else if (message->get_type() == StatusMessage::message_type()) {
    auto status_msg = dynamic_cast<StatusMessage *>(message.get());
    if (status_msg) {
      std::cout << "[BASE APP] Received Status: Level="
                << static_cast<int>(status_msg->get_level())
                << ", Desc=" << status_msg->get_description() << std::endl;
    }
  }
  // Add handlers for other relevant message types (e.g., TelemetryMessage)
}

// Specific handler for Status/Telemetry if using set_status_callback
void handle_status_telemetry(const std::string &rover_id,
                             const std::map<std::string, double> &data) {
  std::cout << "[BASE STATUS CB] Received Status/Telemetry from Rover ID: "
            << rover_id << std::endl;
  for (const auto &pair : data) {
    std::cout << "  " << pair.first << ": " << pair.second << std::endl;
  }
}

int main() {
  boost::asio::io_context io_context;
  std::cout << "[MAIN BASE] Starting Base Station Test Main..." << std::endl;

  // Create the BaseStation instance
  BaseStation base(io_context, LISTEN_PORT, BASE_STATION_ID);
  g_base_station = &base; // Set global pointer for handler access

  // --- Register Custom Handlers ---
  // Register the general application-level handler
  base.set_application_message_handler(handle_base_message);
  std::cout << "[MAIN BASE] Registered application message handler."
            << std::endl;

  // Optionally, register the specific status/telemetry handler
  base.set_status_callback(handle_status_telemetry);
  std::cout << "[MAIN BASE] Registered status/telemetry callback." << std::endl;

  // --- Start Base Station ---
  base.start(); // Starts listening for rover connections
  std::cout << "[MAIN BASE] Base Station started. Listening on port "
            << LISTEN_PORT << "." << std::endl;

  // --- Run io_context in a separate thread ---
  std::thread io_thread([&io_context]() {
    try {
      io_context.run();
    } catch (const std::exception &e) {
      std::cerr << "[ERROR] io_context exception: " << e.what() << std::endl;
    }
    std::cout << "[MAIN BASE] io_context thread finished." << std::endl;
  });

  // --- Keep Running ---
  std::cout << "[MAIN BASE] Base Station running. Waiting for connections and "
               "messages. Press Ctrl+C to exit."
            << std::endl;
  // Keep the main thread alive indefinitely, or use a signal handler for clean
  // shutdown
  io_thread.join(); // In this simple case, join will block until io_context is
                    // stopped

  // --- Stop Base Station and Cleanup (will likely need Ctrl+C to reach here)
  // ---
  std::cout << "[MAIN BASE] Stopping Base Station..." << std::endl;
  base.stop();

  // io_context might already be stopped if io_thread finished, but ensure it
  if (!io_context.stopped()) {
    io_context.stop();
  }
  // Ensure thread is joined if it wasn't already
  if (io_thread.joinable()) {
    io_thread.join();
  }

  std::cout << "[MAIN BASE] Base Station Test Main finished." << std::endl;
  return 0;
}