// extensive_rover_test.cpp
// Advanced test program for the rover component

#include "basic_message.hpp"
#include "command_message.hpp"
#include "rover.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"

#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace po = boost::program_options;
using boost::asio::ip::udp;

// Statistics for the rover
struct RoverStats {
  size_t messages_received = 0;
  size_t basic_messages_received = 0;
  size_t command_messages_received = 0;
  size_t messages_sent = 0;
  size_t telemetry_messages_sent = 0;
  size_t status_messages_sent = 0;
  size_t handshakes_initiated = 0;
  size_t handshakes_completed = 0;
  size_t errors = 0;

  std::map<std::string, size_t> commands_received;
  std::chrono::system_clock::time_point start_time =
      std::chrono::system_clock::now();

  std::mutex stats_mutex;

  void print() {
    std::lock_guard<std::mutex> lock(stats_mutex);

    auto now = std::chrono::system_clock::now();
    auto runtime =
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time)
            .count();

    std::cout << "\n=== Rover Statistics ===" << std::endl;
    std::cout << "Runtime: " << runtime << " seconds" << std::endl;
    std::cout << "Total messages received: " << messages_received << std::endl;
    std::cout << "  Basic messages: " << basic_messages_received << std::endl;
    std::cout << "  Command messages: " << command_messages_received
              << std::endl;
    std::cout << "Messages sent: " << messages_sent << std::endl;
    std::cout << "  Telemetry messages: " << telemetry_messages_sent
              << std::endl;
    std::cout << "  Status messages: " << status_messages_sent << std::endl;
    std::cout << "Handshakes initiated: " << handshakes_initiated << std::endl;
    std::cout << "Handshakes completed: " << handshakes_completed << std::endl;
    std::cout << "Errors encountered: " << errors << std::endl;

    if (!commands_received.empty()) {
      std::cout << "\nCommands received:" << std::endl;
      for (const auto &[cmd, count] : commands_received) {
        std::cout << "  " << cmd << ": " << count << std::endl;
      }
    }
    std::cout << "==============================\n" << std::endl;
  }
};

// Extended rover with additional testing functionality
class TestRover {
public:
  TestRover(boost::asio::io_context &io_context, const std::string &base_host,
            int base_port, const std::string &rover_id, RoverStats &stats)
      : io_context_(io_context), base_host_(base_host), base_port_(base_port),
        rover_id_(rover_id), stats_(stats), running_(false),
        telemetry_timer_(io_context), status_update_timer_(io_context),
        session_check_timer_(io_context),
        message_received_handler_([this](std::unique_ptr<Message> message,
                                         const udp::endpoint &sender) {
          handle_message(std::move(message), sender);
        }),
        random_gen_(std::random_device{}()) {

    std::cout << "[TEST ROVER] Initializing with ID: " << rover_id
              << ", connecting to base at: " << base_host << ":" << base_port
              << std::endl;
  }

  ~TestRover() { stop(); }

  void start() {
    if (running_)
      return;
    running_ = true;

    // Create the rover
    rover_ =
        std::make_unique<Rover>(io_context_, base_host_, base_port_, rover_id_);

    // Start the rover
    rover_->start();

    {
      std::lock_guard<std::mutex> lock(stats_.stats_mutex);
      stats_.handshakes_initiated++;
    }

    std::cout << "[TEST ROVER] Started" << std::endl;

    // Start periodic status checks
    check_session_status();

    // Start stats printer
    schedule_stats_print();
  }

  void stop() {
    if (!running_)
      return;
    running_ = false;

    // Cancel timers
    telemetry_timer_.cancel();
    status_update_timer_.cancel();
    session_check_timer_.cancel();

    if (rover_) {
      rover_->stop();
      rover_.reset();
    }

    std::cout << "[TEST ROVER] Stopped" << std::endl;
  }

  void handle_message(std::unique_ptr<Message> message,
                      const udp::endpoint &sender) {
    std::string msg_type = message->get_type();
    std::string sender_id = message->get_sender();

    {
      std::lock_guard<std::mutex> lock(stats_.stats_mutex);
      stats_.messages_received++;

      if (msg_type == BasicMessage::message_type()) {
        stats_.basic_messages_received++;
      } else if (msg_type == CommandMessage::message_type()) {
        stats_.command_messages_received++;

        // Track command types
        auto cmd_msg = dynamic_cast<CommandMessage *>(message.get());
        if (cmd_msg) {
          std::string cmd = cmd_msg->get_command();
          stats_.commands_received[cmd]++;
        }
      }
    }

    std::cout << "[TEST ROVER] Received message type: " << msg_type
              << " from: " << sender_id << std::endl;

    // Print more details for command messages
    if (msg_type == CommandMessage::message_type()) {
      auto cmd_msg = dynamic_cast<CommandMessage *>(message.get());
      if (cmd_msg) {
        std::cout << "  Command: " << cmd_msg->get_command()
                  << ", Params: " << cmd_msg->get_params() << std::endl;
      }
    } else if (msg_type == BasicMessage::message_type()) {
      auto basic_msg = dynamic_cast<BasicMessage *>(message.get());
      if (basic_msg) {
        std::cout << "  Content: " << basic_msg->get_content() << std::endl;
      }
    }

    // Handle specific commands for testing
    if (msg_type == CommandMessage::message_type()) {
      auto cmd_msg = dynamic_cast<CommandMessage *>(message.get());
      if (cmd_msg) {
        std::string cmd = cmd_msg->get_command();

        // Handle test-specific commands
        if (cmd == "TEST_TELEMETRY_START") {
          start_telemetry_generation(
              parse_int_param(cmd_msg->get_params(), 5000));
        } else if (cmd == "TEST_TELEMETRY_STOP") {
          stop_telemetry_generation();
        } else if (cmd == "TEST_ERROR") {
          simulate_error(cmd_msg->get_params());
        } else if (cmd == "TEST_STATUS_CHANGE") {
          change_status_level(cmd_msg->get_params());
        } else if (cmd == "TEST_RECONNECT") {
          reconnect();
        }
      }
    }
  }

  void start_telemetry_generation(int interval_ms = 5000) {
    if (!running_ || !rover_) {
      std::cerr << "[TEST ROVER] Cannot start telemetry, rover not running"
                << std::endl;
      return;
    }

    // Cancel any existing timer
    telemetry_timer_.cancel();

    telemetry_interval_ms_ = interval_ms;

    // Schedule first telemetry
    schedule_telemetry_send();

    std::cout << "[TEST ROVER] Started telemetry generation (interval: "
              << interval_ms << "ms)" << std::endl;
  }

  void stop_telemetry_generation() {
    telemetry_timer_.cancel();
    std::cout << "[TEST ROVER] Stopped telemetry generation" << std::endl;
  }

  void simulate_error(const std::string &error_type) {
    if (!running_ || !rover_) {
      std::cerr << "[TEST ROVER] Cannot simulate error, rover not running"
                << std::endl;
      return;
    }

    std::cout << "[TEST ROVER] Simulating error: " << error_type << std::endl;

    {
      std::lock_guard<std::mutex> lock(stats_.stats_mutex);
      stats_.errors++;
    }

    // Update rover status to error
    rover_->update_status(StatusMessage::StatusLevel::ERROR,
                          "Simulated error: " + error_type);

    if (error_type == "disconnect") {
      // Stop the rover to simulate disconnection
      stop();

      // Schedule reconnection after a delay
      auto timer = std::make_shared<boost::asio::steady_timer>(
          io_context_, std::chrono::seconds(5));

      timer->async_wait([this, timer](const boost::system::error_code &ec) {
        if (!ec) {
          std::cout
              << "[TEST ROVER] Auto-reconnecting after simulated disconnect"
              << std::endl;
          start();
        }
      });
    } else if (error_type == "packet_loss") {
      // We don't actually implement packet loss since that would require
      // modifying the protocol layer. In a real test, you'd inject packet
      // loss at the UDP level.
      std::cout << "[TEST ROVER] Packet loss simulation not implemented"
                << std::endl;
    }
  }

  void change_status_level(const std::string &level_str) {
    if (!running_ || !rover_) {
      std::cerr << "[TEST ROVER] Cannot change status, rover not running"
                << std::endl;
      return;
    }

    StatusMessage::StatusLevel level = StatusMessage::StatusLevel::OK;
    std::string description;

    if (level_str == "ok" || level_str == "OK") {
      level = StatusMessage::StatusLevel::OK;
      description = "System nominal";
    } else if (level_str == "warning" || level_str == "WARNING") {
      level = StatusMessage::StatusLevel::WARNING;
      description = "Test warning condition";
    } else if (level_str == "error" || level_str == "ERROR") {
      level = StatusMessage::StatusLevel::ERROR;
      description = "Test error condition";
    } else if (level_str == "critical" || level_str == "CRITICAL") {
      level = StatusMessage::StatusLevel::CRITICAL;
      description = "Test critical condition";
    } else {
      std::cerr << "[TEST ROVER] Unknown status level: " << level_str
                << std::endl;
      return;
    }

    rover_->update_status(level, description);

    std::cout << "[TEST ROVER] Changed status to: " << level_str << " - "
              << description << std::endl;

    {
      std::lock_guard<std::mutex> lock(stats_.stats_mutex);
      stats_.status_messages_sent++;
      stats_.messages_sent++;
    }
  }

  void reconnect() {
    if (!running_) {
      std::cerr << "[TEST ROVER] Cannot reconnect, test not running"
                << std::endl;
      return;
    }

    std::cout << "[TEST ROVER] Performing reconnection test" << std::endl;

    // Stop and restart the rover
    if (rover_) {
      rover_->stop();
      rover_.reset();
    }

    // Short delay before reconnecting
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Create and start a new rover instance
    rover_ =
        std::make_unique<Rover>(io_context_, base_host_, base_port_, rover_id_);
    rover_->start();

    {
      std::lock_guard<std::mutex> lock(stats_.stats_mutex);
      stats_.handshakes_initiated++;
    }

    std::cout << "[TEST ROVER] Reconnection initiated" << std::endl;
  }

  void run_automated_test_sequence() {
    if (!running_ || !rover_) {
      std::cerr << "[TEST ROVER] Cannot run test sequence, rover not running"
                << std::endl;
      return;
    }

    std::cout << "[TEST ROVER] Starting automated test sequence" << std::endl;

    // Start telemetry generation
    start_telemetry_generation(3000); // Every 3 seconds

    // Schedule a series of test events
    schedule_status_updates();

    // After 60 seconds, simulate an error
    auto error_timer = std::make_shared<boost::asio::steady_timer>(
        io_context_, std::chrono::seconds(60));

    error_timer->async_wait(
        [this, error_timer](const boost::system::error_code &ec) {
          if (!ec && running_ && rover_) {
            simulate_error("disconnect");
          }
        });

    // After 120 seconds, stop telemetry
    auto stop_timer = std::make_shared<boost::asio::steady_timer>(
        io_context_, std::chrono::seconds(120));

    stop_timer->async_wait(
        [this, stop_timer](const boost::system::error_code &ec) {
          if (!ec && running_) {
            stop_telemetry_generation();
            std::cout << "[TEST ROVER] Automated test sequence completed"
                      << std::endl;
          }
        });
  }

  Rover::SessionState get_session_state() const {
    if (!rover_)
      return Rover::SessionState::INACTIVE;
    return rover_->get_session_state();
  }

  void print_session_status() const {
    if (!rover_) {
      std::cout << "Session status: ROVER NOT INITIALIZED" << std::endl;
      return;
    }

    Rover::SessionState state = rover_->get_session_state();

    std::cout << "Session status: ";
    switch (state) {
    case Rover::SessionState::INACTIVE:
      std::cout << "INACTIVE - Not connected to base station" << std::endl;
      break;
    case Rover::SessionState::HANDSHAKE_INIT:
      std::cout << "HANDSHAKE_INIT - Handshake initiated" << std::endl;
      break;
    case Rover::SessionState::HANDSHAKE_ACCEPT:
      std::cout << "HANDSHAKE_ACCEPT - Handshake in progress" << std::endl;
      break;
    case Rover::SessionState::ACTIVE:
      std::cout << "ACTIVE - Connected to base station" << std::endl;
      if (previous_state_ != Rover::SessionState::ACTIVE) {
        std::lock_guard<std::mutex> lock(stats_.stats_mutex);
        stats_.handshakes_completed++;
      }
      break;
    }

    previous_state_ = state;
  }

private:
  void schedule_telemetry_send() {
    if (!running_ || !rover_)
      return;

    telemetry_timer_.expires_after(
        std::chrono::milliseconds(telemetry_interval_ms_));
    telemetry_timer_.async_wait([this](const boost::system::error_code &ec) {
      if (ec || !running_ || !rover_)
        return;

      // Generate and send random telemetry data
      send_random_telemetry();

      // Schedule next telemetry
      schedule_telemetry_send();
    });
  }

  void schedule_status_updates() {
    if (!running_ || !rover_)
      return;

    // Set up a sequence of status changes
    // We'll start with OK, then to WARNING, then to OK again

    // After 20 seconds, change to WARNING
    auto warning_timer = std::make_shared<boost::asio::steady_timer>(
        io_context_, std::chrono::seconds(20));

    warning_timer->async_wait(
        [this, warning_timer](const boost::system::error_code &ec) {
          if (!ec && running_ && rover_) {
            change_status_level("WARNING");
          }
        });

    // After 40 seconds, change back to OK
    auto ok_timer = std::make_shared<boost::asio::steady_timer>(
        io_context_, std::chrono::seconds(40));

    ok_timer->async_wait([this, ok_timer](const boost::system::error_code &ec) {
      if (!ec && running_ && rover_) {
        change_status_level("OK");
      }
    });
  }

  void send_random_telemetry() {
    if (!running_ || !rover_)
      return;

    // Only send if session is active
    if (rover_->get_session_state() != Rover::SessionState::ACTIVE) {
      return;
    }

    // Generate random sensor readings
    std::map<std::string, double> readings;

    // Random temperature (20-50°C)
    std::uniform_real_distribution<double> temp_dist(20.0, 50.0);
    readings["temperature"] = temp_dist(random_gen_);

    // Random battery level (0-100%)
    std::uniform_real_distribution<double> battery_dist(50.0, 100.0);
    readings["battery"] = battery_dist(random_gen_);

    // Random motor speeds (-100 to 100)
    std::uniform_real_distribution<double> motor_dist(-100.0, 100.0);
    readings["left_motor"] = motor_dist(random_gen_);
    readings["right_motor"] = motor_dist(random_gen_);

    // Random sensor readings
    std::uniform_real_distribution<double> sensor_dist(0.0, 1000.0);
    readings["front_distance"] = sensor_dist(random_gen_);
    readings["left_distance"] = sensor_dist(random_gen_);
    readings["right_distance"] = sensor_dist(random_gen_);

    // Random CPU and memory usage
    std::uniform_real_distribution<double> cpu_dist(0.0, 100.0);
    readings["cpu_load"] = cpu_dist(random_gen_);
    readings["memory_usage"] = cpu_dist(random_gen_);

    // Random network metrics
    std::uniform_real_distribution<double> net_dist(0.0, 200.0);
    readings["network_latency"] = net_dist(random_gen_);
    readings["packet_loss"] =
        std::uniform_real_distribution<double>(0.0, 10.0)(random_gen_);

    // Send the telemetry
    rover_->send_telemetry(readings);

    {
      std::lock_guard<std::mutex> lock(stats_.stats_mutex);
      stats_.telemetry_messages_sent++;
      stats_.messages_sent++;
    }

    std::cout << "[TEST ROVER] Sent telemetry with " << readings.size()
              << " readings" << std::endl;
  }

  int parse_int_param(const std::string &param, int default_value) {
    try {
      return std::stoi(param);
    } catch (const std::exception &) {
      return default_value;
    }
  }

  void check_session_status() {
    if (!running_)
      return;

    print_session_status();

    // Schedule next check
    session_check_timer_.expires_after(std::chrono::seconds(10));
    session_check_timer_.async_wait(
        [this](const boost::system::error_code &ec) {
          if (ec || !running_)
            return;
          check_session_status();
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

      schedule_stats_print();
    });
  }

  boost::asio::io_context &io_context_;
  std::string base_host_;
  int base_port_;
  std::string rover_id_;
  RoverStats &stats_;

  std::unique_ptr<Rover> rover_;
  bool running_;

  boost::asio::steady_timer telemetry_timer_;
  boost::asio::steady_timer status_update_timer_;
  boost::asio::steady_timer session_check_timer_;

  std::function<void(std::unique_ptr<Message>, const udp::endpoint &)>
      message_received_handler_;

  int telemetry_interval_ms_ = 5000;
  std::mt19937 random_gen_;

  // For tracking state changes
  mutable Rover::SessionState previous_state_ = Rover::SessionState::INACTIVE;
};

// Interactive console for controlling the test
class TestConsole {
public:
  TestConsole(TestRover &rover, boost::asio::io_context &io_context)
      : rover_(rover), io_context_(io_context), input_thread_active_(false) {}

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
        rover_.print_session_status();
      } else if (command == "auto") {
        rover_.run_automated_test_sequence();
      } else if (command == "telemetry") {
        std::string subcmd;
        iss >> subcmd;

        if (subcmd == "start") {
          int interval = 5000; // Default: 5 seconds
          iss >> interval;
          rover_.start_telemetry_generation(interval);
        } else if (subcmd == "stop") {
          rover_.stop_telemetry_generation();
        } else {
          std::cout << "Usage: telemetry start [interval_ms] | telemetry stop"
                    << std::endl;
        }
      } else if (command == "error") {
        std::string error_type;
        iss >> error_type;

        if (error_type.empty()) {
          std::cout << "Usage: error <error_type>" << std::endl;
          std::cout << "  error_type: disconnect, packet_loss" << std::endl;
          continue;
        }

        rover_.simulate_error(error_type);
      } else if (command == "status_level") {
        std::string level;
        iss >> level;

        if (level.empty()) {
          std::cout << "Usage: status_level <level>" << std::endl;
          std::cout << "  level: OK, WARNING, ERROR, CRITICAL" << std::endl;
          continue;
        }

        rover_.change_status_level(level);
      } else if (command == "reconnect") {
        rover_.reconnect();
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
    std::cout << "  auto                 - Run automated test sequence"
              << std::endl;
    std::cout << "  telemetry start [interval_ms] - Start telemetry generation"
              << std::endl;
    std::cout << "  telemetry stop       - Stop telemetry generation"
              << std::endl;
    std::cout << "  error <type>         - Simulate an error (disconnect, "
                 "packet_loss)"
              << std::endl;
    std::cout << "  status_level <level> - Change status level (OK, WARNING, "
                 "ERROR, CRITICAL)"
              << std::endl;
    std::cout << "  reconnect            - Force reconnection to base station"
              << std::endl;
  }

  TestRover &rover_;
  boost::asio::io_context &io_context_;
  std::thread input_thread_;
  bool input_thread_active_;
};

int main(int argc, char *argv[]) {
  try {
    // Parse command line options
    po::options_description desc("Allowed options");
    desc.add_options()("help,h", "produce help message")(
        "host,H", po::value<std::string>()->default_value("localhost"),
        "base station host")("port,p", po::value<int>()->default_value(8080),
                             "base station port")(
        "id,i", po::value<std::string>()->default_value("test-rover"),
        "rover ID")("auto,a", "start automated test sequence immediately");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
      std::cout << desc << std::endl;
      return 0;
    }

    std::string base_host = vm["host"].as<std::string>();
    int base_port = vm["port"].as<int>();
    std::string rover_id = vm["id"].as<std::string>();
    bool auto_start = vm.count("auto") > 0;

    // Create IO context
    boost::asio::io_context io_context;

    // Set up signal handling for graceful shutdown
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&io_context](const boost::system::error_code &, int) {
      std::cout << "Shutting down rover test..." << std::endl;
      io_context.stop();
    });

    std::cout << "Starting extensive rover test" << std::endl;
    std::cout << "Base station: " << base_host << ":" << base_port << std::endl;
    std::cout << "Rover ID: " << rover_id << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;

    // Create statistics tracker
    RoverStats stats;

    // Create test rover
    TestRover test_rover(io_context, base_host, base_port, rover_id, stats);

    // Start the rover
    test_rover.start();

    // Start the console
    TestConsole console(test_rover, io_context);
    console.start();

    // Start automated sequence if requested
    if (auto_start) {
      test_rover.run_automated_test_sequence();
    }

    // Run the IO context
    io_context.run();

    // Clean up
    console.stop();
    test_rover.stop();

    // Show final statistics
    stats.print();

  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}