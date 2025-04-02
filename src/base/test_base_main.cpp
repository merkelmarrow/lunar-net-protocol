#include "base_station.hpp"
#include "message.hpp" // For Message base class and pretty_print
#include <boost/asio/io_context.hpp>
#include <iostream>
#include <memory> // For std::unique_ptr

int main() {
  const int LISTEN_PORT = 9000; // Port for the base station to listen on
  const std::string STATION_ID = "test-base";

  try {
    boost::asio::io_context io_context;

    // Create the BaseStation instance
    BaseStation base(io_context, LISTEN_PORT, STATION_ID);

    // Set a simple handler to print received messages
    base.set_application_message_handler(
        [&](std::unique_ptr<Message> message,
            const boost::asio::ip::udp::endpoint &sender) {
          if (!message)
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

    // Start the base station
    base.start();

    std::cout << "[BASE MAIN] Base station started on port " << LISTEN_PORT
              << "." << std::endl;
    std::cout << "[BASE MAIN] Waiting for connections..." << std::endl;

    // Run the io_context to handle asynchronous operations
    io_context.run();

    std::cout << "[BASE MAIN] io_context stopped." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[BASE MAIN] Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}