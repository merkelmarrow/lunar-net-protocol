#include "message.hpp" // For Message base class and pretty_print
#include "rover.hpp"
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono> // For std::chrono::seconds
#include <iostream>
#include <memory> // For std::unique_ptr
#include <thread> // For std::this_thread::sleep_for

int main() {
  const std::string BASE_HOST = "10.237.0.201"; // Target Base Station IP
  const int BASE_PORT = 9000; // Target Base Station Port (must match base)
  const std::string ROVER_ID = "test-rover";

  try {
    boost::asio::io_context io_context;

    // Create the Rover instance [cite: 637]
    Rover rover(io_context, BASE_HOST, BASE_PORT, ROVER_ID);

    // Set a simple handler to print received messages and check session state
    // [cite: 645]
    rover.set_application_message_handler(
        [&](std::unique_ptr<Message> message,
            const boost::asio::ip::udp::endpoint &sender) {
          if (!message)
            return;
          std::cout << "[ROVER MAIN] Received message type '"
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

          // Check session state after receiving messages (especially
          // session-related ones)
          Rover::SessionState state = rover.get_session_state();
          std::cout << "[ROVER MAIN] Current Session State: ";
          switch (state) {
          case Rover::SessionState::INACTIVE:
            std::cout << "INACTIVE";
            break;
          case Rover::SessionState::HANDSHAKE_INIT:
            std::cout << "HANDSHAKE_INIT";
            break;
          case Rover::SessionState::HANDSHAKE_ACCEPT:
            std::cout << "HANDSHAKE_ACCEPT";
            break;
          case Rover::SessionState::ACTIVE:
            std::cout << "ACTIVE";
            break;
          default:
            std::cout << "UNKNOWN";
            break;
          }
          std::cout << std::endl;
        });

    // Start the rover - this initiates the handshake
    rover.start();

    std::cout << "[ROVER MAIN] Rover started. Attempting handshake with "
              << BASE_HOST << ":" << BASE_PORT << "." << std::endl;

    // Run the io_context to handle asynchronous operations
    // For this simple test, it will run until explicitly stopped or runs out of
    // work. In a real application, you might run it in a separate thread or use
    // io_context.run_for().
    io_context.run();

    std::cout << "[ROVER MAIN] io_context stopped." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[ROVER MAIN] Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}