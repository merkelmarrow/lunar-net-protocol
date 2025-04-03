#include "basic_message.hpp"
#include "command_message.hpp"
#include "message.hpp"
#include "rover.hpp"
#include "telemetry_message.hpp"
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
const int BASE_PORT = 9000;

int main() {
  const std::string BASE_HOST = "10.237.0.201";

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

          // if basic message just print to terminal
          if (message->get_type() == BasicMessage::message_type()) {
            BasicMessage *basic_msg =
                dynamic_cast<BasicMessage *>(message.get());
            if (basic_msg) {
              std::cout << Message::pretty_print(basic_msg->serialise())
                        << std::endl;
            }
          }
        });

    rover.start();
    rover.update_current_position(53.3498, -6.2603);

    std::cout << "[ROVER MAIN] Rover started. Attempting handshake with "
              << BASE_HOST << ":" << BASE_PORT << "." << std::endl;

    std::vector<boost::asio::ip::udp::endpoint> received_endpoints;
    auto extra_listener =
        std::make_shared<UdpServer>(io_context, EXTRA_LISTENER_PORT);
    auto coords_listener =
        std::make_shared<UdpServer>(io_context, COORD_REQUEST_TARGET_PORT);

    extra_listener->set_receive_callback(
        [&rover, &received_endpoints,
         &ROVER_ID](const std::vector<uint8_t> &data,
                    const boost::asio::ip::udp::endpoint &sender) {
          std::cout << "[Listener " << EXTRA_LISTENER_PORT << "] Received "
                    << data.size() << " bytes from " << sender << std::endl;

          std::string received_msg(data.begin(), data.end());
          std::cout << "[Listener " << EXTRA_LISTENER_PORT
                    << "] Raw Message: " << received_msg << std::endl;

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

          BasicMessage response_msg("Acknowledged. We are group 18.", ROVER_ID);

          std::cout << "[Listener " << EXTRA_LISTENER_PORT
                    << "] Sending ACK message back to " << sender << std::endl;
          std::cout << "-> \"Acknowledged. We are group 18.\"\n";
          rover.send_raw_message(response_msg, sender);
        });
    coords_listener->set_receive_callback(
        [&rover, &received_endpoints,
         &ROVER_ID](const std::vector<uint8_t> &data,
                    const boost::asio::ip::udp::endpoint &sender) {
          std::cout << "[Listener " << COORD_REQUEST_TARGET_PORT
                    << "] Received " << data.size() << " bytes from " << sender
                    << std::endl;

          std::string received_msg(data.begin(), data.end());
          std::cout << "[Listener " << COORD_REQUEST_TARGET_PORT
                    << "] Raw Message: " << received_msg << std::endl;

          std::map<std::string, double> location = {{"latitude", 53.3498},
                                                    {"longitude", -6.2603}};

          TelemetryMessage location_msg(location, ROVER_ID);

          std::cout << "[Listener " << COORD_REQUEST_TARGET_PORT
                    << "] Sending location message back to " << sender
                    << std::endl;
          rover.send_raw_message(location_msg, sender);
        });

    extra_listener->start();
    coords_listener->start();

    std::cout << "[ROVER MAIN] Started extra listener on port "
              << EXTRA_LISTENER_PORT << "." << std::endl;

    std::cout << "[ROVER MAIN] Started coords listener on port "
              << COORD_REQUEST_TARGET_PORT << "." << std::endl;

    // Coordinate request timer
    boost::asio::steady_timer request_timer(io_context);
    boost::asio::steady_timer broadcast_timer(io_context);

    std::function<void(const boost::system::error_code &)>
        broadcast_timer_handler;
    std::function<void(const boost::system::error_code &)>
        request_timer_handler;

    request_timer_handler = [&](const boost::system::error_code &ec) {
      if (ec == boost::asio::error::operation_aborted) {
        std::cout << "[Coord Timer] Timer cancelled." << std::endl;
        return;
      } else if (ec) {
        std::cerr << "[Coord Timer] Timer error: " << ec.message() << std::endl;
        return;
      }

      if (rover.is_low_power_mode()) {
        std::cout
            << "[Coord Timer] Skipping coordinate requests (Low Power Mode)."
            << std::endl;
      } else {
        std::cout << "[Coord Timer] Sending coordinate requests..."
                  << std::endl;

        for (const auto &ep : received_endpoints) {
          // Create the target endpoint with the correct port
          boost::asio::ip::udp::endpoint target_ep(ep.address(),
                                                   COORD_REQUEST_TARGET_PORT);

          CommandMessage cmd_msg("SEND_DATA", "LOCATION", ROVER_ID);
          rover.send_raw_message(cmd_msg, target_ep);
        }
      }

      request_timer.expires_after(std::chrono::seconds(17));
      request_timer.async_wait(request_timer_handler);
    };

    broadcast_timer_handler = [&](const boost::system::error_code &ec) {
      if (ec == boost::asio::error::operation_aborted) {
        std::cout << "[Broadcast Timer] Timer cancelled." << std::endl;
        return;
      } else if (ec) {
        std::cerr << "[Broadcast Timer] Timer error: " << ec.message()
                  << std::endl;
        return;
      }

      if (rover.is_low_power_mode()) {
        std::cout << "[Broadcast Timer] Skipping periodic broadcast (Low "
                     "Power Mode)."
                  << std::endl;
      } else {
        std::cout << "[Broadcast Timer] Sending periodic broadcast..."
                  << std::endl;

        if (extra_listener) {
          extra_listener->scan_for_rovers(EXTRA_LISTENER_PORT, "ACK IF ALIVE",
                                          ROVER_ID);
        }
      }

      broadcast_timer.expires_after(std::chrono::seconds(23));
      broadcast_timer.async_wait(broadcast_timer_handler);
    };

    broadcast_timer.expires_at(std::chrono::steady_clock::now());
    broadcast_timer.async_wait(broadcast_timer_handler);
    std::cout << "[ROVER MAIN] Periodic broadcast timer started." << std::endl;

    // Start the timer for the first time
    request_timer.expires_after(std::chrono::seconds(14));
    request_timer.async_wait(request_timer_handler);
    std::cout << "[ROVER MAIN] Coordinate request timer started." << std::endl;

    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code &, int /*signal_number*/) {
          std::cout << "Interrupt signal received. Stopping..." << std::endl;
          request_timer.cancel();
          broadcast_timer.cancel();
          if (extra_listener)
            extra_listener->stop();
          if (coords_listener)
            coords_listener->stop();
          rover.stop();
          io_context.stop();
        });

    std::cout << "[ROVER MAIN] Rover, listener, and timer running. Press "
                 "Ctrl+C to stop."
              << std::endl;

    io_context.run();

    std::cout << "[ROVER MAIN] io_context stopped." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[ROVER MAIN] Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}