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

    // Use a timer to schedule additional message sending after the handshake
    // completes.
    boost::asio::steady_timer timer(io_context, std::chrono::seconds(5));
    timer.async_wait([&](const boost::system::error_code &ec) {
      if (!ec) {
        // --- Send Telemetry ---
        std::map<std::string, double> telemetry = {{"temperature", 23.5},
                                                   {"voltage", 12.2}};
        rover.send_telemetry(telemetry);
        std::cout << "[TEST] Telemetry message sent." << std::endl;

        // --- Update and Send Status ---
        rover.update_status(StatusMessage::StatusLevel::WARNING, "Low battery");
        rover.send_status();
        std::cout << "[TEST] Status message sent." << std::endl;

        // --- Create and Send a Raw JSON Message ---
        // Here we create a BasicMessage which serializes to JSON.
        BasicMessage rawMsg("Raw JSON message from rover", rover_id);
        // Create an endpoint for the base station.
        boost::asio::ip::udp::endpoint base_endpoint(
            boost::asio::ip::address::from_string(base_ip), base_port);
        rover.send_raw_message(rawMsg, base_endpoint);
        std::cout << "[TEST] Raw JSON message sent." << std::endl;
      }
    });

    // Run the io_context event loop.
    io_context.run();
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Exception in Rover: " << e.what() << std::endl;
  }
  return 0;
}
