#include "base_station.hpp"
#include <boost/asio.hpp>
#include <iostream>
#include <map>

int main() {
  try {
    boost::asio::io_context io_context;
    int port = 9000; // The port on which the base station will listen.
    std::string station_id = "BaseStation-01";

    // Create the BaseStation instance.
    BaseStation baseStation(io_context, port, station_id);

    // Set a status callback to print telemetry/status updates received from a
    // rover.
    baseStation.set_status_callback(
        [](const std::string &rover_id,
           const std::map<std::string, double> &status) {
          std::cout << "[STATUS CALLBACK] Received update from rover "
                    << rover_id << ": ";
          for (const auto &entry : status) {
            std::cout << entry.first << "=" << entry.second << " ";
          }
          std::cout << std::endl;
        });

    baseStation.start();
    std::cout << "[MAIN] Base Station started on port " << port
              << " on IP 10.237.0.201. Waiting for rover connections..."
              << std::endl;

    // Run the io_context event loop.
    io_context.run();
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] Exception in BaseStation: " << e.what() << std::endl;
  }
  return 0;
}
