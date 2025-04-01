#include "base_station.hpp"
#include "basic_message.hpp" // Include BasicMessage
#include "command_message.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <thread> // For std::this_thread::sleep_for

// --- Application-Level Callbacks Defined in Main ---

// 1. Specific handler for Status and Telemetry data
void handle_rover_status_telemetry(const std::string &rover_id,
                                   const std::map<std::string, double> &data) {
  std::cout << "\n---- [STATUS/TELEMETRY CALLBACK] ----" << std::endl;
  std::cout << "  Rover ID: " << rover_id << std::endl;
  std::cout << "  Data: ";
  for (const auto &entry : data) {
    std::cout << entry.first << "=" << entry.second << "; ";
  }
  std::cout << "\n-------------------------------------\n" << std::endl;
  // Add logic here, e.g., log to database, update UI
}

// 2. General handler for other application messages from the rover
void handle_generic_rover_message(std::unique_ptr<Message> message,
                                  const udp::endpoint &sender) {
  if (!message)
    return;

  std::cout << "\n>>>> [GENERIC APP HANDLER] Received Message >>>>"
            << std::endl;
  std::cout << "  Type:    " << message->get_type() << std::endl;
  std::cout << "  Sender:  " << message->get_sender() << std::endl;
  std::cout << "  From IP: " << sender << std::endl;
  std::cout << "  Timestamp: "
            << tp_utils::tp_to_string(message->get_timestamp()) << std::endl;

  // Handle specific types if needed
  if (message->get_type() == BasicMessage::message_type()) {
    auto basic_msg = dynamic_cast<BasicMessage *>(message.get());
    if (basic_msg) {
      std::cout << "  Content: " << basic_msg->get_content() << std::endl;
      std::cout << "  >> Received basic message from rover." << std::endl;
    }
  }
  // Example: Handle a potential command response FROM the rover
  else if (message->get_type() == CommandMessage::message_type()) {
    auto cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    if (cmd_msg) {
      std::cout << "  Command: " << cmd_msg->get_command() << std::endl;
      std::cout << "  Params:  " << cmd_msg->get_params() << std::endl;
      if (cmd_msg->get_command() == "ACK_TEST_COMMAND") {
        std::cout << "  >> Rover acknowledged our test command!" << std::endl;
      } else {
        std::cout << "  >> Received unexpected command from rover."
                  << std::endl;
      }
    }
  }
  // Add handlers for other expected application message types from the rover
  else {
    std::cout << "  >> Received unhandled message type in generic app handler."
              << std::endl;
    try {
      std::cout << "  Raw JSON:\n"
                << Message::pretty_print(message->serialise()) << std::endl;
    } catch (...) { /* ignore */
    }
  }
  std::cout << "<<<< [GENERIC APP HANDLER] Finished Processing <<<<\n"
            << std::endl;
}

int main() {
  try {
    boost::asio::io_context io_context;
    int port = 9000; // Port for the base station
    std::string station_id = "BaseStation-App";

    std::cout << "[MAIN] Creating BaseStation..." << std::endl;
    BaseStation base_station(io_context, port, station_id);

    // --- Setup Application Callbacks ---

    // 1. Set the specific callback for Status/Telemetry
    base_station.set_status_callback(handle_rover_status_telemetry);

    // 2. Set the general handler for other application messages
    base_station.set_application_message_handler(handle_generic_rover_message);

    // --- Timer to periodically send commands (example) ---
    boost::asio::steady_timer command_timer(io_context);
    std::function<void(const boost::system::error_code &)>
        send_command_periodically;
    int command_counter = 0;

    send_command_periodically = [&](const boost::system::error_code &ec) {
      if (ec == boost::asio::error::operation_aborted)
        return;
      if (ec) {
        std::cerr << "[ERROR] Command timer error: " << ec.message()
                  << std::endl;
        return;
      }

      if (base_station.get_session_state() ==
          BaseStation::SessionState::ACTIVE) {
        std::string cmd = "AUTO_CMD_" + std::to_string(++command_counter);
        std::string params = "Triggered by timer";
        std::cout << "\n==== [MAIN] Sending Command ====" << std::endl;
        std::cout << "  To Rover: " << base_station.get_connected_rover_id()
                  << std::endl;
        std::cout << "  Command:  " << cmd << std::endl;
        std::cout << "  Params:   " << params << std::endl;
        base_station.send_command(cmd, params);
        std::cout << "==============================\n" << std::endl;

      } else {
        std::cout << "[MAIN] Command timer: No active rover session."
                  << std::endl;
      }

      // Reschedule
      command_timer.expires_after(
          std::chrono::seconds(25)); // Send command every 25s
      command_timer.async_wait(send_command_periodically);
    };

    // Start command timer (wait longer initially)
    command_timer.expires_after(std::chrono::seconds(15));
    command_timer.async_wait(send_command_periodically);
    std::cout << "[MAIN] Periodic command timer scheduled." << std::endl;

    // --- Start the Base Station ---
    base_station.start();
    std::cout << "[MAIN] Base Station started on port " << port
              << ". Waiting for connections..." << std::endl;

    // --- Graceful Shutdown Handling ---
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code &error, int signal_number) {
          if (!error) {
            std::cout << "\n[MAIN] Signal " << signal_number
                      << " received. Shutting down..." << std::endl;
            command_timer.cancel();
            base_station.stop();
            io_context.stop();
          }
        });

    // --- Run the io_context ---
    std::thread io_thread([&]() {
      try {
        io_context.run();
      } catch (const std::exception &e) {
        std::cerr << "[ERROR] Exception in io_context thread: " << e.what()
                  << std::endl;
        command_timer.cancel();
        base_station.stop();
      }
    });
    std::cout << "[MAIN] io_context running in a separate thread." << std::endl;
    io_thread.join();
    std::cout << "[MAIN] Base Station finished." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Exception in main: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}