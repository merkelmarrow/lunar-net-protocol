#include "basic_message.hpp"
#include "rover.hpp"
#include "status_message.hpp"
#include <boost/asio.hpp>
#include <chrono>
#include <iostream>
#include <map>

int main() {
  try {
    boost::asio::io_context io_context;
    // Base station details: IP 10.237.0.201 and port 9000.
    std::string base_ip = "10.237.0.201";
    int base_port = 9000;
    std::string rover_id = "grp18-rover";

    // Create the Rover instance.
    Rover rover(io_context, base_ip, base_port, rover_id);

    rover.start();
    std::cout << "[MAIN] Rover started on IP 10.237.0.47, connecting to Base "
                 "Station at "
              << base_ip << ":" << base_port << std::endl;

    // Use a timer to check if the session is active before sending messages
    boost::asio::steady_timer check_session_timer(io_context,
                                                  std::chrono::seconds(1));

    // Define an async recurring function to check session state
    std::function<void(const boost::system::error_code &)> check_session;

    check_session = [&](const boost::system::error_code &ec) {
      if (!ec) {
        if (rover.get_session_state() == Rover::SessionState::ACTIVE) {
          // Session is active, send messages
          std::map<std::string, double> telemetry = {{"temperature", 23.5},
                                                     {"voltage", 12.2}};
          rover.send_telemetry(telemetry);
          std::cout << "[TEST] Telemetry message sent." << std::endl;

          // Update and Send Status
          rover.update_status(StatusMessage::StatusLevel::WARNING,
                              "Low battery");
          rover.send_status();
          std::cout << "[TEST] Status message sent." << std::endl;

          // Create and Send a Raw JSON Message
          BasicMessage rawMsg("Raw JSON message from rover", rover_id);
          boost::asio::ip::udp::endpoint base_endpoint(
              boost::asio::ip::address::from_string(base_ip), base_port);
          rover.send_raw_message(rawMsg, base_endpoint);
          std::cout << "[TEST] Raw JSON message sent." << std::endl;
        } else {
          // Session not active yet, reschedule check
          std::cout << "[TEST] Waiting for session to become active..."
                    << std::endl;
          check_session_timer.expires_after(std::chrono::seconds(1));
          check_session_timer.async_wait(check_session);
        }
      }
    };

    // Start checking session state
    check_session_timer.async_wait(check_session);

    // Run the io_context event loop.
    io_context.run();
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Exception in Rover: " << e.what() << std::endl;
  }
  return 0;
}
