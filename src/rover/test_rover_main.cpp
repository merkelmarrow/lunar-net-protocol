#include "basic_message.hpp"
#include "message.hpp"
#include "rover.hpp"
#include "udp_server.hpp"

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <iostream>
#include <memory>
#include <vector>

const int EXTRA_LISTENER_PORT = 60060;

int main() {
  const std::string BASE_HOST = "10.237.0.201";
  const int BASE_PORT = 9000;
  const std::string ROVER_ID = "grp-18";

  try {
    boost::asio::io_context io_context;

    Rover rover(io_context, BASE_HOST, BASE_PORT, ROVER_ID);

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
        });

    rover.start();

    std::cout << "[ROVER MAIN] Rover started. Attempting handshake with "
              << BASE_HOST << ":" << BASE_PORT << "." << std::endl;

    std::vector<boost::asio::ip::udp::endpoint> received_endpoints;
    auto extra_listener =
        std::make_shared<UdpServer>(io_context, EXTRA_LISTENER_PORT);

    extra_listener->set_receive_callback(

        [&rover, &received_endpoints,
         &ROVER_ID](const std::vector<uint8_t> &data,
                    const boost::asio::ip::udp::endpoint &sender) {
          std::cout << "[Listener " << EXTRA_LISTENER_PORT << "] Received "
                    << data.size() << " bytes from " << sender << std::endl;

          std::string received_msg(data.begin(), data.end());
          std::cout << "[Listener " << EXTRA_LISTENER_PORT
                    << "] Raw Message: " << received_msg << std::endl;

          received_endpoints.push_back(sender);
          std::cout << "[Listener " << EXTRA_LISTENER_PORT
                    << "] Stored endpoint. Total stored: "
                    << received_endpoints.size() << std::endl;

          BasicMessage response_msg("Acknowledged. We are group 18.", ROVER_ID);

          std::cout << "[Listener " << EXTRA_LISTENER_PORT
                    << "] Sending message back to " << sender << std::endl;
          std::cout << "-> \"Acknowledged. We are group 18.\"";
          rover.send_raw_message(response_msg, sender);
        });

    extra_listener->start();
    std::cout << "[ROVER MAIN] Started extra listener on port "
              << EXTRA_LISTENER_PORT << "." << std::endl;

    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code &, int /*signal_number*/) {
          std::cout << "Interrupt signal received. Stopping..." << std::endl;
          extra_listener->stop();
          rover.stop();
          io_context.stop();
        });

    std::cout
        << "[ROVER MAIN] Rover and listener running. Press Ctrl+C to stop."
        << std::endl;
    io_context.run();

    std::cout << "[ROVER MAIN] io_context stopped." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[ROVER MAIN] Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}