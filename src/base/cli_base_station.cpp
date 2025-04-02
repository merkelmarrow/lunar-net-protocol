// src/base/cli_base_station.cpp

#include "base_station.hpp"    // Your BaseStation class header
#include "command_message.hpp" // To send commands
#include <boost/asio/io_context.hpp>
#include <atomic>    // For std::atomic<bool> to control CLI shutdown safely
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>    // For std::vector to store input tokens

// Helper function: Splits a string into tokens based on whitespace.
std::vector<std::string> split(const std::string &input) {
    std::istringstream iss(input);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

// CLI loop function that runs in its own thread.
void run_cli(BaseStation &baseStation, std::atomic<bool> &cliRunning) {
    std::string line;
    std::cout << "Base Station CLI started. Type 'help' for commands, 'exit' to quit." 
              << std::endl;
    while (cliRunning && std::getline(std::cin, line)) {
        if (line.empty())
            continue;

        auto tokens = split(line);
        std::string command = tokens[0];

        // Handle built-in commands.
        if (command == "exit" || command == "quit") {
            cliRunning = false;
            std::cout << "Exiting CLI..." << std::endl;
            break;
        }
        if (command == "help") {
            std::cout << "Available commands:" << std::endl
                      << "  MOVE_FORWARD <distance>" << std::endl
                      << "  STOP" << std::endl
                      << "  COLLECT_SAMPLE" << std::endl
                      << "  help    - Show this help message" << std::endl
                      << "  exit    - Quit the CLI" << std::endl;
            continue;
        }

        // Concatenate any additional tokens as the parameter string.
        std::string params;
        if (tokens.size() > 1) {
            std::ostringstream oss;
            for (size_t i = 1; i < tokens.size(); ++i) {
                oss << tokens[i];
                if (i < tokens.size() - 1)
                    oss << " ";
            }
            params = oss.str();
        }

        // Send the command using the BaseStation's send_command method.
        baseStation.send_command(command, params);
        std::cout << "[CLI] Command '" << command << "' with parameters '"
                  << params << "' sent." << std::endl;
    }
}

int main() {
    try 
    {
        // Create the io_context and BaseStation instance.
        boost::asio::io_context io_context;

        const std::string BASE_ID = "BaseStation01";

        BaseStation baseStation[0](io_context, 9001, BASE_ID);
        BaseStation baseStation[1](io_context, 9002, BASE_ID);
        BaseStation baseStation[2](io_context, 9003, BASE_ID);
        BaseStation baseStation[3](io_context, 9004, BASE_ID);

        // Optionally, register a handler to process responses from the rover.
        baseStation[i].set_application_message_handler([](std::unique_ptr<Message> msg, const udp::endpoint &sender) 
        {
            std::cout << "[BASE APP] Received message from " << sender
                      << ", Type: " << msg->get_type() << std::endl
                      << "Message: " << msg->serialise() << std::endl;
                      
        });

        for(int i = 0; i < 4; i++)
        {
            // Start the base station.
            baseStation[i].start();

            std::cout << "[BASE CLI] Base Station started on port " << BASE_PORT 
                    << ". You can now issue commands via CLI." << std::endl;

            // Launch the CLI in a separate thread.
            std::atomic<bool> cliRunning{true};
            std::thread cliThread(run_cli, std::ref(baseStation[i]), std::ref(cliRunning));

            // Run the io_context to process network events.
            io_context.run();

            // When the io_context stops, signal the CLI to exit.
            cliRunning = false;
            if (cliThread.joinable())
                cliThread.join();

            baseStation[i].stop();
            std::cout << "[BASE CLI] Base Station stopped cleanly." << std::endl;
        }
    } 
    catch (const std::exception &ex) 
    {
        std::cerr << "[BASE CLI] Exception: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
