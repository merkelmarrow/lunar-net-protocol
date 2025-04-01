// src/rover/test_rover_main.cpp
#include "basic_message.hpp" // To send a basic message
#include "command_message.hpp"
#include "rover.hpp"
#include <boost/asio.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread> // For std::this_thread

const std::string BASE_HOST = "10.237.0.201"; // Base station IP
const int BASE_PORT = 9001;                   // Base station port
const std::string ROVER_ID = "test-rover-01";

// Custom application message handler for the Rover
void handle_rover_message(std::unique_ptr<Message> message,
                          const udp::endpoint &sender) {
  if (!message) {
    std::cout << "[ROVER APP] Received null message pointer." << std::endl;
    return;
  }
  std::cout << "[ROVER APP] Received message type: " << message->get_type()
            << " from sender: " << message->get_sender() << " at endpoint "
            << sender << std::endl;

  // Example: Handle a BasicMessage received from the base
  if (message->get_type() == BasicMessage::message_type()) {
    auto basic_msg = dynamic_cast<BasicMessage *>(message.get());
    if (basic_msg) {
      std::cout << "[ROVER APP] Received BasicMessage content: "
                << basic_msg->get_content() << std::endl;
      // Could potentially send another message back here if needed
    }
  }
  // Add handlers for other expected message types if necessary
}

int main() {
  boost::asio::io_context io_context;
  std::cout << "[MAIN ROVER] Starting Rover Test Main..." << std::endl;

  // Create the Rover instance
  Rover rover(io_context, BASE_HOST, BASE_PORT, ROVER_ID);

  // --- Register Custom Handlers ---
  // Register the application-level handler
  rover.set_application_message_handler(handle_rover_message);
  std::cout << "[MAIN ROVER] Registered application message handler."
            << std::endl;

  // --- Start Rover ---
  rover.start(); // This initiates the handshake internally
  std::cout << "[MAIN ROVER] Rover started. Waiting for handshake..."
            << std::endl;

  // --- Run io_context in a separate thread ---
  std::thread io_thread([&io_context]() {
    try {
      io_context.run();
    } catch (const std::exception &e) {
      std::cerr << "[ERROR] io_context exception: " << e.what() << std::endl;
    }
    std::cout << "[MAIN ROVER] io_context thread finished." << std::endl;
  });

  // --- Wait for Handshake Completion ---
  int wait_count = 0;
  const int max_wait = 20; // Wait up to 20 seconds for handshake
  while (rover.get_session_state() != Rover::SessionState::ACTIVE &&
         wait_count < max_wait) {
    std::cout << "[MAIN ROVER] Waiting for ACTIVE state (current: "
              << static_cast<int>(rover.get_session_state()) << ")"
              << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    wait_count++;
  }

  if (rover.get_session_state() == Rover::SessionState::ACTIVE) {
    std::cout << "[MAIN ROVER] Handshake successful! Session is ACTIVE."
              << std::endl;

    // --- Send a Message ---
    std::cout << "[MAIN ROVER] Sending a BasicMessage to the base..."
              << std::endl;
    BasicMessage test_msg("Hello from Rover!", ROVER_ID);
    // Send message using the rover's send mechanism (which uses MessageManager)
    // We don't need the endpoint here as Rover sends to its registered base
    // Note: The underlying MessageManager::send_message will handle the
    // endpoint
    rover.send_message(test_msg);

    // --- Keep Running for a bit ---
    std::cout << "[MAIN ROVER] Rover running. Sending periodic status. Press "
                 "Ctrl+C to exit."
              << std::endl;
    // Let it run for a while to potentially receive messages
    std::this_thread::sleep_for(std::chrono::seconds(30));

  } else {
    std::cerr << "[MAIN ROVER] Handshake failed or timed out. Current state: "
              << static_cast<int>(rover.get_session_state()) << std::endl;
  }

  // --- Stop Rover and Cleanup ---
  std::cout << "[MAIN ROVER] Stopping rover..." << std::endl;
  rover.stop();

  std::cout << "[MAIN ROVER] Stopping io_context..." << std::endl;
  io_context.stop();

  if (io_thread.joinable()) {
    io_thread.join();
  }

  std::cout << "[MAIN ROVER] Rover Test Main finished." << std::endl;
  return 0;
}