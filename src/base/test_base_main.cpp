#include "base_station.hpp"
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <iomanip>
#include <iostream>
#include <map>

// Callback function to handle status/telemetry data
void handle_status(const std::string &rover_id,
                   const std::map<std::string, double> &data) {
  std::cout << "=== Received data from rover: " << rover_id
            << " ===" << std::endl;

  for (const auto &[key, value] : data) {
    std::cout << "  " << std::left << std::setw(12) << key << ": " << std::fixed
              << std::setprecision(2) << value << std::endl;
  }
  std::cout << std::endl;
}

int main() {
  try {
    // Create IO context
    boost::asio::io_context io_context;

    // Set up signal handling for graceful shutdown
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&io_context](const boost::system::error_code &, int) {
      std::cout << "Shutting down base station server..." << std::endl;
      io_context.stop();
    });

    // Port to listen on
    int port = 8080;

    std::cout << "Starting base station test server" << std::endl;
    std::cout << "Local IP: 10.237.0.201" << std::endl;
    std::cout << "Listening on port " << port << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;

    // Create the base station
    BaseStation base_station(io_context, port, "test-base-1");

    // Set the callback for status/telemetry data
    base_station.set_status_callback(handle_status);

    // Start the base station (begins listening for connections)
    base_station.start();

    // Status printer timer
    boost::asio::steady_timer status_timer(io_context,
                                           std::chrono::seconds(10));

    std::function<void(const boost::system::error_code &)> print_status;
    print_status = [&](const boost::system::error_code &error) {
      if (error) {
        return; // Timer cancelled or error
      }

      BaseStation::SessionState state = base_station.get_session_state();
      std::string rover_id = base_station.get_connected_rover_id();

      std::cout << "Connection status: ";
      switch (state) {
      case BaseStation::SessionState::INACTIVE:
        std::cout << "INACTIVE - Waiting for rover connection" << std::endl;
        break;
      case BaseStation::SessionState::HANDSHAKE_INIT:
        std::cout << "HANDSHAKE_INIT - Handshake in progress" << std::endl;
        break;
      case BaseStation::SessionState::HANDSHAKE_ACCEPT:
        std::cout << "HANDSHAKE_ACCEPT - Handshake in progress" << std::endl;
        break;
      case BaseStation::SessionState::ACTIVE:
        std::cout << "ACTIVE - Connected to rover: " << rover_id << std::endl;
        break;
      }

      // Reschedule the timer
      status_timer.expires_at(status_timer.expiry() + std::chrono::seconds(10));
      status_timer.async_wait(print_status);
    };

    // Start the status timer
    status_timer.async_wait(print_status);

    // Run the IO context (this blocks until io_context.stop() is called)
    io_context.run();

    // Clean up
    base_station.stop();

  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}