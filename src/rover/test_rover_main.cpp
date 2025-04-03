// lunar-net-protocol-cpy/src/rover/test_rover_main.cpp

#include "basic_message.hpp"
#include "command_message.hpp"
#include "message.hpp"
#include "rover.hpp"
#include "udp_server.hpp"

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

const int EXTRA_LISTENER_PORT = 60060;
const int COORD_REQUEST_TARGET_PORT = 50050;

int main() {
  const std::string BASE_HOST =
      "10.237.0.201"; // Using localhost for local demo
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

          if (message->get_type() == CommandMessage::message_type()) {
            CommandMessage *cmd_msg =
                dynamic_cast<CommandMessage *>(message.get());
            if (cmd_msg && cmd_msg->get_command() == "GET_STATUS") {
              std::cout << "[ROVER MAIN] Received GET_STATUS command from base."
                        << std::endl;
              std::cout << "[ROVER MAIN] --> Responding with current status..."
                        << std::endl;
              rover.send_status(); // Send current status back to base
            } else if (cmd_msg) {
              // Handle other commands if necessary
              std::cout << "[ROVER MAIN] Received other command: "
                        << cmd_msg->get_command() << std::endl;
              std::cout << Message::pretty_print(cmd_msg->serialise())
                        << std::endl;
            }
          }
          // Handle other message types if needed
          else if (message->get_type() == BasicMessage::message_type()) {
            BasicMessage *basic_msg =
                dynamic_cast<BasicMessage *>(message.get());
            if (basic_msg) {
              std::cout << "[ROVER MAIN] Received BasicMessage:" << std::endl;
              std::cout << Message::pretty_print(basic_msg->serialise())
                        << std::endl;
            }
          }
          // Implicitly handles Telemetry/Status (logged by Base)
        });

    rover.start();

    std::cout << "[ROVER MAIN] Rover started. Attempting handshake with "
              << BASE_HOST << ":" << BASE_PORT << "." << std::endl;

    std::vector<boost::asio::ip::udp::endpoint>
        received_endpoints; // For discovery demo
    auto extra_listener =
        std::make_shared<UdpServer>(io_context, EXTRA_LISTENER_PORT);

    extra_listener->set_receive_callback(
        [&rover, &received_endpoints,
         &ROVER_ID](const std::vector<uint8_t> &data,
                    const boost::asio::ip::udp::endpoint &sender) {
          std::string received_msg(data.begin(), data.end());
          std::cout << "[Listener " << EXTRA_LISTENER_PORT
                    << "] Received probe/message: \"" << received_msg
                    << "\" from " << sender << std::endl;

          if (received_msg.find("ACK IF ALIVE") != std::string::npos ||
              received_msg.find("ROVER_DISCOVER") !=
                  std::string::npos) { // Check for probe keywords
            BasicMessage response_msg(
                "Acknowledged: Rover " + ROVER_ID + " is active.", ROVER_ID);
            std::cout << "[Listener " << EXTRA_LISTENER_PORT
                      << "] --> Sending basic info ACK back to " << sender
                      << std::endl;
            rover.send_raw_message(response_msg,
                                   sender); // Send basic info back directly
          } else {
            std::cout << "[Listener " << EXTRA_LISTENER_PORT
                      << "] Received non-probe message. Not sending ACK."
                      << std::endl;
          }

          // Store endpoint if new
          bool found = false;
          for (const auto &ep : received_endpoints) {
            if (ep == sender) {
              found = true;
              break;
            }
          }
          if (!found) {
            received_endpoints.push_back(sender);
            std::cout << "[Listener " << EXTRA_LISTENER_PORT
                      << "] Stored new endpoint " << sender
                      << ". Total stored: " << received_endpoints.size()
                      << std::endl;
          }
        });

    extra_listener->start();

    std::cout << "[ROVER MAIN] Started extra listener on port "
              << EXTRA_LISTENER_PORT << " for simple discovery/probes."
              << std::endl;

    // Timers for periodic actions
    boost::asio::steady_timer broadcast_timer(io_context);

    std::function<void(const boost::system::error_code &)>
        broadcast_timer_handler;

    broadcast_timer_handler = [&](const boost::system::error_code &ec) {
      if (ec == boost::asio::error::operation_aborted) {
        std::cout << "[Broadcast Timer] Timer cancelled." << std::endl;
        return;
      } else if (ec) {
        std::cerr << "[Broadcast Timer] Timer error: " << ec.message()
                  << std::endl;
        return;
      }

      std::cout << "[Broadcast Timer] Sending discovery broadcast/scan..."
                << std::endl;

      extra_listener->scan_for_rovers(
          EXTRA_LISTENER_PORT, "ACK IF ALIVE",
          ROVER_ID); // Option 2: Use listener's simple broadcast

      broadcast_timer.expires_after(
          std::chrono::seconds(23)); // Interval for broadcast
      broadcast_timer.async_wait(broadcast_timer_handler);
    };

    broadcast_timer.expires_at(std::chrono::steady_clock::now());
    broadcast_timer.async_wait(broadcast_timer_handler);
    std::cout << "[ROVER MAIN] Periodic broadcast timer started." << std::endl;

    // Signal handling for clean shutdown
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code &, int /*signal_number*/) {
          std::cout << "Interrupt signal received. Stopping..." << std::endl;
          broadcast_timer.cancel();
          // request_timer.cancel(); // Uncomment if using request_timer
          if (extra_listener)
            extra_listener->stop();
          rover.stop();
          io_context.stop();
        });

    std::cout
        << "[ROVER MAIN] Rover setup complete. Running... Press Ctrl+C to stop."
        << std::endl;

    io_context.run();

    std::cout << "[ROVER MAIN] io_context stopped." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[ROVER MAIN] Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}