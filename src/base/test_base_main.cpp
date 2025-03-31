// extensive_base_test.cpp
// Advanced test program for the base station component

#include "base_station.hpp"
#include "basic_message.hpp"
#include "command_message.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"

#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>

namespace po = boost::program_options;
using boost::asio::ip::udp;

// Statistics for the base station
struct BaseStationStats {
  size_t messages_received = 0;
  size_t basic_messages_received = 0;
  size_t command_messages_received = 0;
  size_t status_messages_received = 0;
  size_t telemetry_messages_received = 0;
  size_t messages_sent = 0;
  size_t handshakes_completed = 0;
  size_t errors = 0;

  std::map<std::string, size_t> rovers_seen;
  std::chrono::system_clock::time_point start_time =
      std::chrono::system_clock::now();

  std::mutex stats_mutex;

  void print() {
    std::lock_guard<std::mutex> lock(stats_mutex);

    auto now = std::chrono::system_clock::now();
    auto runtime =
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time)
            .count();

    std::cout << "\n=== Base Station Statistics ===" << std::endl;
    std::cout << "Runtime: " << runtime << " seconds" << std::endl;
    std::cout << "Total messages received: " << messages_received << std::endl;
    std::cout << "  Basic messages: " << basic_messages_received << std::endl;
    std::cout << "  Command messages: " << command_messages_received
              << std::endl;
    std::cout << "  Status messages: " << status_messages_received << std::endl;
    std::cout << "  Telemetry messages: " << telemetry_messages_received
              << std::endl;
    std::cout << "Messages sent: " << messages_sent << std::endl;
    std::cout << "Handshakes completed: " << handshakes_completed << std::endl;
    std::cout << "Errors encountered: " << errors << std::endl;

    std::cout << "\nRovers seen:" << std::endl;
    for (const auto &[rover_id, count] : rovers_seen) {
      std::cout << "  " << rover_id << ": " << count << " messages"
                << std::endl;
    }
    std::cout << "==============================\n" << std::endl;
  }
};

// Custom message handler for the base station
class TestBaseMessageHandler {
public:
  TestBaseMessageHandler(BaseStationStats &stats) : stats_(stats) {}

  void handle_message(const std::string &rover_id,
                      const std::map<std::string, double> &data) {
    std::lock_guard<std::mutex> lock(stats_.stats_mutex);

    // Update statistics
    stats_.messages_received++;
    stats_.rovers_seen[rover_id]++;

    // Print the message data
    std::cout << "=== Received data from rover: " << rover_id
              << " ===" << std::endl;

    for (const auto &[key, value] : data) {
      std::cout << "  " << std::left << std::setw(12) << key << ": "
                << std::fixed << std::setprecision(2) << value << std::endl;
    }
    std::cout << std::endl;
  }

private:
  BaseStationStats &stats_;
};

// Extended base station with additional testing functionality
class TestBaseStation {
public:
  TestBaseStation(boost::asio::io_context &io_context, int port,
                  const std::string &station_id, BaseStationStats &stats)
      : io_context_(io_context), base_station_(io_context, port, station_id),
        stats_(stats), cmd_timer_(io_context),
        random_gen_(std::random_device{}()), running_(false) {

    message_handler_ = std::make_unique<TestBaseMessageHandler>(stats);
    base_station_.set_status_callback(
        [this](const std::string &rover_id,
               const std::map<std::string, double> &data) {
          message_handler_->handle_message(rover_id, data);

          // Track message types
          std::lock_guard<std::mutex> lock(stats_.stats_mutex);

          // Detect if this is a status message based on the presence of
          // "status_level" key
          if (data.find("status_level") != data.end()) {
            stats_.status_messages_received++;
          } else {
            stats_.telemetry_messages_received++;
          }
        });
  }

  ~TestBaseStation() { stop(); }

  void start() {
    if (running_)
      return;
    running_ = true;

    base_station_.start();
    std::cout << "[TEST BASE] Base station started on port " << port_
              << std::endl;

    // Start stats printer
    schedule_stats_print();
  }

  void stop() {
    if (!running_)
      return;
    running_ = false;

    // Cancel timers
    cmd_timer_.cancel();

    base_station_.stop();
    std::cout << "[TEST BASE] Base station stopped" << std::endl;
  }

  void send_test_command(const std::string &command, const std::string &params,
                         const udp::endpoint &endpoint) {
    if (!running_) {
      std::cerr << "[TEST BASE] Cannot send command, base station not running"
                << std::endl;
      return;
    }

    CommandMessage cmd_msg(command, params, "test-base");
    base_station_.send_raw_message(cmd_msg, endpoint);

    std::lock_guard<std::mutex> lock(stats_.stats_mutex);
    stats_.messages_sent++;

    std::cout << "[TEST BASE] Sent command: " << command
              << " with params: " << params << " to " << endpoint << std::endl;
  }

  void send_test_basic_message(const std::string &content,
                               const udp::endpoint &endpoint) {
    if (!running_) {
      std::cerr << "[TEST BASE] Cannot send message, base station not running"
                << std::endl;
      return;
    }

    BasicMessage msg(content, "test-base");
    base_station_.send_raw_message(msg, endpoint);

    std::lock_guard<std::mutex> lock(stats_.stats_mutex);
    stats_.messages_sent++;

    std::cout << "[TEST BASE] Sent basic message: " << content << " to "
              << endpoint << std::endl;
  }

  void start_random_command_sequence(const udp::endpoint &endpoint,
                                     int interval_ms = 10000,
                                     int duration_sec = 60) {
    if (!running_) {
      std::cerr << "[TEST BASE] Cannot start command sequence, base station "
                   "not running"
                << std::endl;
      return;
    }

    // Reset timer
    cmd_timer_.cancel();

    // Set up end time
    auto end_time =
        std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);

    // Schedule first command
    schedule_random_command(endpoint, interval_ms, end_time);

    std::cout << "[TEST BASE] Started random command sequence to " << endpoint
              << " (interval: " << interval_ms
              << "ms, duration: " << duration_sec << "s)" << std::endl;
  }

  BaseStation::SessionState get_session_state() const {
    return base_station_.get_session_state();
  }

  std::string get_connected_rover_id() const {
    return base_station_.get_connected_rover_id();
  }

  void print_session_status() const {
    BaseStation::SessionState state = base_station_.get_session_state();
    std::string rover_id = base_station_.get_connected_rover_id();

    std::cout << "Session status: ";
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
      if (previous_state_ != BaseStation::SessionState::ACTIVE) {
        std::lock_guard<std::mutex> lock(stats_.stats_mutex);
        stats_.handshakes_completed++;
      }
      break;
    }

    previous_state_ = state;
  }

private:
  void schedule_random_command(const udp::endpoint &endpoint, int interval_ms,
                               std::chrono::steady_clock::time_point end_time) {
    if (!running_)
      return;

    cmd_timer_.expires_after(std::chrono::milliseconds(interval_ms));
    cmd_timer_.async_wait([this, endpoint, interval_ms,
                           end_time](const boost::system::error_code &ec) {
      if (ec || !running_)
        return;

      // Check if we've reached the end time
      if (std::chrono::steady_clock::now() >= end_time) {
        std::cout << "[TEST BASE] Random command sequence completed"
                  << std::endl;
        return;
      }

      // Generate a random command
      std::uniform_int_distribution<int> dist(0, 3);
      int cmd_type = dist(random_gen_);

      switch (cmd_type) {
      case 0: {
        // Motor command
        std::uniform_real_distribution<double> speed_dist(-100.0, 100.0);
        double left_speed = speed_dist(random_gen_);
        double right_speed = speed_dist(random_gen_);
        std::string params =
            std::to_string(left_speed) + "," + std::to_string(right_speed);
        send_test_command("MOTOR", params, endpoint);
        break;
      }
      case 1: {
        // Camera command
        std::uniform_int_distribution<int> cam_dist(0, 2);
        std::string cmd;
        switch (cam_dist(random_gen_)) {
        case 0:
          cmd = "PAN";
          break;
        case 1:
          cmd = "TILT";
          break;
        case 2:
          cmd = "ZOOM";
          break;
        }
        std::uniform_real_distribution<double> value_dist(-45.0, 45.0);
        std::string params = std::to_string(value_dist(random_gen_));
        send_test_command("CAMERA_" + cmd, params, endpoint);
        break;
      }
      case 2: {
        // Sensor command
        std::uniform_int_distribution<int> sensor_dist(0, 2);
        std::string cmd;
        switch (sensor_dist(random_gen_)) {
        case 0:
          cmd = "ENABLE";
          break;
        case 1:
          cmd = "DISABLE";
          break;
        case 2:
          cmd = "CALIBRATE";
          break;
        }
        send_test_command("SENSOR_" + cmd, "", endpoint);
        break;
      }
      case 3: {
        // System command
        std::uniform_int_distribution<int> sys_dist(0, 2);
        std::string cmd;
        switch (sys_dist(random_gen_)) {
        case 0:
          cmd = "RESTART";
          break;
        case 1:
          cmd = "PING";
          break;
        case 2:
          cmd = "DIAGNOSTIC";
          break;
        }
        send_test_command("SYSTEM_" + cmd, "", endpoint);
        break;
      }
      }

      // Schedule next command
      schedule_random_command(endpoint, interval_ms, end_time);
    });
  }

  void schedule_stats_print() {
    if (!running_)
      return;

    // Print stats every 30 seconds
    auto timer = std::make_shared<boost::asio::steady_timer>(
        io_context_, std::chrono::seconds(30));
    timer->async_wait([this, timer](const boost::system::error_code &ec) {
      if (ec || !running_)
        return;

      stats_.print();
      print_session_status();

      schedule_stats_print();
    });
  }

  boost::asio::io_context &io_context_;
  BaseStation base_station_;
  BaseStationStats &stats_;
  std::unique_ptr<TestBaseMessageHandler> message_handler_;
  boost::asio::steady_timer cmd_timer_;

  std::mt19937 random_gen_;
  int port_ = 8080;
  bool running_;

  // For tracking state changes
  mutable BaseStation::SessionState previous_state_ =
      BaseStation::SessionState::INACTIVE;
};

// Interactive console for controlling the test
class TestConsole {
public:
  TestConsole(TestBaseStation &base, boost::asio::io_context &io_context)
      : base_(base), io_context_(io_context), input_thread_active_(false) {}

  ~TestConsole() { stop(); }

  void start() {
    if (input_thread_active_)
      return;

    input_thread_active_ = true;
    input_thread_ = std::thread([this]() { input_loop(); });

    std::cout << "\n=== Test Console Started ===" << std::endl;
    print_help();
  }

  void stop() {
    if (!input_thread_active_)
      return;

    input_thread_active_ = false;
    if (input_thread_.joinable()) {
      input_thread_.join();
    }
  }

private:
  void input_loop() {
    udp::endpoint target_endpoint;
    bool endpoint_set = false;

    while (input_thread_active_) {
      std::cout << "\nCommand> ";
      std::string line;
      std::getline(std::cin, line);

      if (line.empty())
        continue;

      // Parse the command
      std::istringstream iss(line);
      std::string command;
      iss >> command;

      if (command == "help" || command == "?") {
        print_help();
      } else if (command == "quit" || command == "exit") {
        std::cout << "Stopping test..." << std::endl;
        io_context_.stop();
        break;
      } else if (command == "status") {
        base_.print_session_status();
      } else if (command == "target") {
        std::string host;
        int port;
        iss >> host >> port;

        if (host.empty() || port <= 0) {
          std::cout << "Usage: target <host> <port>" << std::endl;
          continue;
        }

        try {
          udp::resolver resolver(io_context_);
          auto endpoints =
              resolver.resolve(udp::v4(), host, std::to_string(port));
          target_endpoint = *endpoints.begin();
          endpoint_set = true;
          std::cout << "Target set to " << target_endpoint << std::endl;
        } catch (const std::exception &e) {
          std::cerr << "Error resolving target: " << e.what() << std::endl;
        }
      } else if (command == "cmd") {
        if (!endpoint_set) {
          std::cout << "Set a target first with 'target <host> <port>'"
                    << std::endl;
          continue;
        }

        std::string cmd_type, params;
        iss >> cmd_type;

        // Get the rest of the line as params
        std::getline(iss >> std::ws, params);

        if (cmd_type.empty()) {
          std::cout << "Usage: cmd <command_type> [params]" << std::endl;
          continue;
        }

        base_.send_test_command(cmd_type, params, target_endpoint);
      } else if (command == "msg") {
        if (!endpoint_set) {
          std::cout << "Set a target first with 'target <host> <port>'"
                    << std::endl;
          continue;
        }

        std::string content;
        std::getline(iss >> std::ws, content);

        if (content.empty()) {
          std::cout << "Usage: msg <content>" << std::endl;
          continue;
        }

        base_.send_test_basic_message(content, target_endpoint);
      } else if (command == "auto") {
        if (!endpoint_set) {
          std::cout << "Set a target first with 'target <host> <port>'"
                    << std::endl;
          continue;
        }

        int interval = 10000; // Default: 10 seconds
        int duration = 60;    // Default: 1 minute

        iss >> interval >> duration;

        base_.start_random_command_sequence(target_endpoint, interval,
                                            duration);
      } else {
        std::cout << "Unknown command: " << command << std::endl;
        print_help();
      }
    }
  }

  void print_help() {
    std::cout << "Available commands:" << std::endl;
    std::cout << "  help, ?              - Show this help" << std::endl;
    std::cout << "  quit, exit           - Exit the test" << std::endl;
    std::cout << "  status               - Show session status" << std::endl;
    std::cout << "  target <host> <port> - Set target endpoint" << std::endl;
    std::cout << "  cmd <type> [params]  - Send a command message" << std::endl;
    std::cout << "  msg <content>        - Send a basic message" << std::endl;
    std::cout
        << "  auto [interval] [duration] - Start automatic command sequence"
        << std::endl;
    std::cout << "                        interval in ms, duration in seconds"
              << std::endl;
  }

  TestBaseStation &base_;
  boost::asio::io_context &io_context_;
  std::thread input_thread_;
  bool input_thread_active_;
};

int main(int argc, char *argv[]) {
  try {
    // Parse command line options
    po::options_description desc("Allowed options");
    desc.add_options()("help,h", "produce help message")(
        "port,p", po::value<int>()->default_value(8080), "port to listen on")(
        "id,i", po::value<std::string>()->default_value("test-base-station"),
        "base station ID");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
      std::cout << desc << std::endl;
      return 0;
    }

    int port = vm["port"].as<int>();
    std::string base_id = vm["id"].as<std::string>();

    // Create IO context
    boost::asio::io_context io_context;

    // Set up signal handling for graceful shutdown
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&io_context](const boost::system::error_code &, int) {
      std::cout << "Shutting down base station test..." << std::endl;
      io_context.stop();
    });

    std::cout << "Starting extensive base station test" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "Base ID: " << base_id << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;

    // Create statistics tracker
    BaseStationStats stats;

    // Create test base station
    TestBaseStation test_base(io_context, port, base_id, stats);

    // Start the base station
    test_base.start();

    // Start the console
    TestConsole console(test_base, io_context);
    console.start();

    // Run the IO context
    io_context.run();

    // Clean up
    console.stop();
    test_base.stop();

    // Show final statistics
    stats.print();

  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}