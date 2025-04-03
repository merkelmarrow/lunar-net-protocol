// lunar-net-protocol-cpy/src/base/test_base_main.cpp

#include "base_station.hpp"
#include "basic_message.hpp"
#include "command_message.hpp"
#include "message.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <iostream>
#include <memory>

int main() {
  const int LISTEN_PORT = 9000;
  const std::string STATION_ID = "grp-18";

  try {
    boost::asio::io_context io_context;

    // Create the BaseStation instance
    BaseStation base(io_context, LISTEN_PORT, STATION_ID);

    base.set_application_message_handler(
        [&](std::unique_ptr<Message> message,
            const boost::asio::ip::udp::endpoint &sender) {
          if (!message)
            return;

          std::string sender_str = sender.address().to_string() + ":" +
                                   std::to_string(sender.port());

          if (message->get_type() == TelemetryMessage::message_type()) {

            TelemetryMessage *telemetry_msg =
                dynamic_cast<TelemetryMessage *>(message.get());
            std::cout << "[BASE MAIN] <<< Received TELEMETRY from "
                      << sender_str << " (Rover: " << message->get_sender()
                      << ")" << std::endl;
            if (telemetry_msg) {
              std::cout << "  Readings: ";
              for (const auto &[key, value] : telemetry_msg->get_readings()) {
                std::cout << key << "=" << value << "; ";
              }
              std::cout << std::endl;
            }
          } else if (message->get_type() == StatusMessage::message_type()) {

            StatusMessage *status_msg =
                dynamic_cast<StatusMessage *>(message.get());
            std::cout << "[BASE MAIN] <<< Received STATUS from " << sender_str
                      << " (Rover: " << message->get_sender() << ")"
                      << std::endl;
            if (status_msg) {
              std::string level_str;
              switch (status_msg->get_level()) {
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
              default:
                level_str = "UNKNOWN";
                break;
              }
              std::cout << "  Level: " << level_str << ", Desc: \""
                        << status_msg->get_description() << "\"" << std::endl;
            }
          } else if (message->get_type() == CommandMessage::message_type()) {

            std::cout << "[BASE MAIN] <<< Received COMMAND from " << sender_str
                      << " (Rover: " << message->get_sender() << ")"
                      << std::endl;
            std::cout << "  Content:\n"
                      << Message::pretty_print(message->serialise())
                      << std::endl;
          } else if (message->get_type() == BasicMessage::message_type()) {

            std::cout << "[BASE MAIN] <<< Received BASIC MESSAGE from "
                      << sender_str << " (Rover: " << message->get_sender()
                      << ")" << std::endl;
            std::cout << "  Content:\n"
                      << Message::pretty_print(message->serialise())
                      << std::endl;
          } else {
            // Fallback for other types
            std::cout << "[BASE MAIN] <<< Received message type '"
                      << message->get_type() << "' from " << sender_str
                      << " (Rover: " << message->get_sender() << ")"
                      << std::endl;
            try {
              std::cout << "  Content:\n"
                        << Message::pretty_print(message->serialise())
                        << std::endl;
            } catch (const std::exception &e) {
              std::cerr << "  Error pretty-printing message: " << e.what()
                        << std::endl;
            }
          }
        });

    boost::asio::steady_timer command_timer(io_context);
    std::function<void(const boost::system::error_code &)>
        command_timer_handler;

    command_timer_handler = [&](const boost::system::error_code &ec) {
      if (ec == boost::asio::error::operation_aborted) {
        std::cout << "[Command Timer] Timer cancelled." << std::endl;
        return;
      } else if (ec) {
        std::cerr << "[Command Timer] Timer error: " << ec.message()
                  << std::endl;
        // Decide whether to reschedule or stop
      }

      // Only send command if session is active
      if (base.get_session_state() == BaseStation::SessionState::ACTIVE) {
        std::string rover_id = base.get_connected_rover_id();
        std::cout << "[Command Timer] >>> Sending GET_STATUS command to rover: "
                  << rover_id << std::endl;

        base.send_command("GET_STATUS", ""); // Send GET_STATUS command
      } else {
        std::cout << "[Command Timer] Session not active. Command not sent."
                  << std::endl;
      }

      // Reschedule the timer
      command_timer.expires_after(
          std::chrono::seconds(20)); // Send command every 20 seconds
      command_timer.async_wait(command_timer_handler);
    };

    // Start the command timer after a short delay
    command_timer.expires_after(std::chrono::seconds(10));
    command_timer.async_wait(command_timer_handler);
    std::cout << "[BASE MAIN] Periodic command timer started (will send when "
                 "session is active)."
              << std::endl;

    // Start the base station
    base.start();

    std::cout << "[BASE MAIN] Base station started on port " << LISTEN_PORT
              << "." << std::endl;
    std::cout << "[BASE MAIN] Waiting for connections and messages..."
              << std::endl;

    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code &, int /*signal_number*/) {
          std::cout << "Interrupt signal received. Stopping..." << std::endl;
          command_timer.cancel(); // Stop the command timer
          base.stop();
          io_context.stop();
        });

    // Run the io_context to handle asynchronous operations
    io_context.run();

    std::cout << "[BASE MAIN] io_context stopped." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[BASE MAIN] Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}