#include "base_station.hpp"
#include "command_message.hpp"
#include <boost/asio.hpp>
#include <chrono>
#include <iostream>

int main() {
  try {
    boost::asio::io_context io_context;
    int port = 9000; // Base station listening port.
    std::string station_id = "BaseStation-01";

    // Create the BaseStation instance.
    BaseStation baseStation(io_context, port, station_id);

    // Set a status callback to print any telemetry/status updates from a rover.
    baseStation.set_status_callback(
        [](const std::string &rover_id,
           const std::map<std::string, double> &data) {
          std::cout << "[BASE CALLBACK] Received data from rover " << rover_id
                    << ": ";
          for (const auto &entry : data) {
            std::cout << entry.first << "=" << entry.second << " ";
          }
          std::cout << std::endl;
        });

    baseStation.start();
    std::cout << "[MAIN] Base Station started on port " << port
              << ". Waiting for rover connections..." << std::endl;

    // Schedule a timer to send a test command message after 10 seconds.
    boost::asio::steady_timer timer(io_context, std::chrono::seconds(10));
    timer.async_wait([&](const boost::system::error_code &ec) {
      if (!ec) {
        // Create a test command message.
        CommandMessage cmdMsg("TEST_CMD", "param1=value1", station_id);
        // For testing, we create a dummy endpoint.
        // (In a real session this would be the rover's endpoint from the
        // handshake.)
        boost::asio::ip::udp::endpoint rover_endpoint(
            boost::asio::ip::address::from_string("127.0.0.1"), 10000);
        baseStation.send_raw_message(cmdMsg, rover_endpoint);
        std::cout << "[TEST] Test command message sent from base station."
                  << std::endl;
      }
    });

    io_context.run();
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Exception in base station extended test: " << e.what()
              << std::endl;
  }
  return 0;
}
