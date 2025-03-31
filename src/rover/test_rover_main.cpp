#include "rover.hpp"
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <chrono>
#include <iostream>
#include <map>

int main() {
  try {
    // Create IO context
    boost::asio::io_context io_context;

    // Set up signal handling for graceful shutdown
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&io_context](const boost::system::error_code &, int) {
      std::cout << "Shutting down rover client..." << std::endl;
      io_context.stop();
    });

    // Base station connection details
    std::string base_ip = "10.237.0.201"; // Base station IP
    int base_port = 8080;                 // Port to connect to

    std::cout << "Starting rover test client" << std::endl;
    std::cout << "Local IP: 10.237.0.47" << std::endl;
    std::cout << "Connecting to base at " << base_ip << ":" << base_port
              << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;

    // Create the rover
    Rover rover(io_context, base_ip, base_port, "test-rover-1");

    // Start the rover (initiates connection to base station)
    rover.start();

    // Set up a timer to periodically send telemetry
    boost::asio::steady_timer timer(io_context, std::chrono::seconds(5));

    std::function<void(const boost::system::error_code &)> send_telemetry;
    send_telemetry = [&](const boost::system::error_code &error) {
      if (error) {
        return; // Timer cancelled or error
      }

      // Only send if session is active
      if (rover.get_session_state() == Rover::SessionState::ACTIVE) {
        // Generate telemetry data
        std::map<std::string, double> telemetry_data = {{"temperature", 45.5},
                                                        {"battery", 78.9},
                                                        {"speed", 15.3},
                                                        {"cpu_load", 22.7}};

        std::cout << "Sending telemetry data..." << std::endl;
        rover.send_telemetry(telemetry_data);
      } else {
        std::cout << "Waiting for session to become active..." << std::endl;
      }

      // Reschedule the timer
      timer.expires_at(timer.expiry() + std::chrono::seconds(5));
      timer.async_wait(send_telemetry);
    };

    // Start the timer
    timer.async_wait(send_telemetry);

    // Run the IO context (this blocks until io_context.stop() is called)
    io_context.run();

    // Clean up
    rover.stop();

  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}