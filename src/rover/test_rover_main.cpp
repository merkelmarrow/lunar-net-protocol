#include "basic_message.hpp" // For sending raw messages
#include "command_message.hpp"
#include "rover.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <chrono>
#include <functional> // For std::function
#include <iostream>
#include <map>
#include <memory> // For std::unique_ptr
#include <thread> // For std::this_thread::sleep_for

// APPLICATION-LEVEL message handler for messages received from BaseStation
void handle_application_message(std::unique_ptr<Message> message,
                                const udp::endpoint &sender) {
  if (!message)
    return;

  std::cout << "\n<<<< [APP HANDLER] Received Message <<<<" << std::endl;
  std::cout << "  Type:    " << message->get_type() << std::endl;
  std::cout << "  Sender:  " << message->get_sender() << std::endl;
  std::cout << "  From IP: " << sender << std::endl;
  std::cout << "  Timestamp: "
            << tp_utils::tp_to_string(message->get_timestamp()) << std::endl;

  // Handle specific message types
  if (message->get_type() == CommandMessage::message_type()) {
    auto cmd_msg = dynamic_cast<CommandMessage *>(message.get());
    if (cmd_msg) {
      std::cout << "  Command: " << cmd_msg->get_command() << std::endl;
      std::cout << "  Params:  " << cmd_msg->get_params() << std::endl;
      // --- Add application logic for commands ---
      if (cmd_msg->get_command() == "TEST_COMMAND") {
        std::cout << "  >> Rover acknowledging TEST_COMMAND received from base!"
                  << std::endl;
        // Example: Send a response back (careful not to create loops)
        // rover.send_command("ACK_TEST_COMMAND", "Received OK"); // Need access
        // to rover object
      } else {
        std::cout << "  >> Received unhandled command in app handler."
                  << std::endl;
      }
    }
  } else if (message->get_type() == BasicMessage::message_type()) {
    auto basic_msg = dynamic_cast<BasicMessage *>(message.get());
    if (basic_msg) {
      std::cout << "  Content: " << basic_msg->get_content() << std::endl;
      std::cout << "  >> Received basic message from base." << std::endl;
    }
  }
  // Add handlers for other expected application message types (e.g., config
  // updates)
  else {
    std::cout << "  >> Received unhandled message type in app handler."
              << std::endl;
    try {
      std::cout << "  Raw JSON:\n"
                << Message::pretty_print(message->serialise()) << std::endl;
    } catch (...) { /* ignore if not serializable */
    }
  }
  std::cout << "<<<< [APP HANDLER] Finished Processing <<<<\n" << std::endl;
}

int main() {
  try {
    boost::asio::io_context io_context;
    // --- Configuration ---
    std::string base_ip = "10.237.0.201"; // Use localhost for local testing
    int base_port = 9000;
    std::string rover_id = "grp18-rover-app"; // Use a distinct ID

    std::cout << "[MAIN] Creating Rover..." << std::endl;
    Rover rover(io_context, base_ip, base_port, rover_id);

    // --- Setup Application Callback ---
    rover.set_application_message_handler(handle_application_message);
    std::cout << "[MAIN] Application message handler set." << std::endl;

    // --- Start the Rover ---
    rover.start(); // This initiates the handshake
    std::cout << "[MAIN] Rover started. Attempting connection to " << base_ip
              << ":" << base_port << std::endl;

    // --- Timer to Periodically Send Data (once connected) ---
    boost::asio::steady_timer data_send_timer(io_context);
    std::function<void(const boost::system::error_code &)>
        send_data_periodically;

    send_data_periodically = [&](const boost::system::error_code &ec) {
      if (ec == boost::asio::error::operation_aborted)
        return;
      if (ec) {
        std::cerr << "[ERROR] Data send timer error: " << ec.message()
                  << std::endl;
        return;
      }

      if (rover.get_session_state() == Rover::SessionState::ACTIVE) {
        std::cout << "\n---- [MAIN] Sending Data ----" << std::endl;
        // 1. Send Telemetry
        std::map<std::string, double> telemetry = {
            {"temp", 24.5 + (rand() % 20) / 10.0},
            {"volt", 12.1 + (rand() % 6) / 10.0},
            {"sig", -60.0 - (rand() % 15)}};
        rover.send_telemetry(telemetry);
        std::cout << "[MAIN] Telemetry sent: temp=" << telemetry["temp"]
                  << ", volt=" << telemetry["volt"] << std::endl;

        // 2. Update and Send Status (Rover class handles periodic sending via
        // timer)
        if (rand() % 8 == 0) {
          rover.update_status(StatusMessage::StatusLevel::WARNING,
                              "Path deviation detected");
        } else {
          rover.update_status(StatusMessage::StatusLevel::OK,
                              "On planned route");
        }
        // rover.send_status(); // Can still force send if needed, but internal
        // timer handles it

        // 3. Send a Basic Message (Raw JSON) - Less common, used for testing
        // maybe
        static int raw_counter = 0;
        BasicMessage rawMsg("Rover raw msg #" + std::to_string(++raw_counter),
                            rover_id);
        udp::endpoint base_endpoint;
        try {
          boost::asio::ip::udp::resolver resolver(io_context);
          base_endpoint =
              *resolver.resolve(udp::v4(), base_ip, std::to_string(base_port))
                   .begin();
          rover.send_raw_message(rawMsg, base_endpoint);
          std::cout << "[MAIN] Raw JSON message sent." << std::endl;
        } catch (const std::exception &resolve_err) {
          std::cerr
              << "[ERROR] Could not resolve base endpoint for raw message: "
              << resolve_err.what() << std::endl;
        }

        std::cout << "---- [MAIN] Data Sending Cycle Done ---- \n" << std::endl;

      } else {
        std::cout
            << "[MAIN] Waiting for session to become active... Current state: "
            << static_cast<int>(rover.get_session_state()) << std::endl;
      }

      // Reschedule the timer
      data_send_timer.expires_after(
          std::chrono::seconds(10)); // Send data every 10 seconds
      data_send_timer.async_wait(send_data_periodically);
    };

    // Start the data sending timer (wait a bit for handshake)
    data_send_timer.expires_after(std::chrono::seconds(7));
    data_send_timer.async_wait(send_data_periodically);
    std::cout << "[MAIN] Data sending timer scheduled." << std::endl;

    // --- Graceful Shutdown Handling ---
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code &error, int signal_number) {
          if (!error) {
            std::cout << "\n[MAIN] Signal " << signal_number
                      << " received. Shutting down..." << std::endl;
            data_send_timer.cancel();
            rover.stop();
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
        // Optionally try stopping components again
        data_send_timer.cancel();
        rover.stop();
      }
    });
    std::cout << "[MAIN] io_context running in a separate thread." << std::endl;
    io_thread.join();
    std::cout << "[MAIN] Rover finished." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Exception in main: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}