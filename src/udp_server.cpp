#include <iostream>
#include <boost/asio.hpp>
#include <unordered_set>
#include <nlohmann/json.hpp> // JSON handling

using json = nlohmann::json;
using boost::asio::ip::udp;

const int SERVER_PORT = 5000; // UDP server listens on this port
std::unordered_set<int> received_packets; // Store sequence numbers to track missing ones

void process_message(const std::string& message,udp::socket& socket,udp::endpoint sender_endpoint) {
    try {
        json received_json = json::parse(message);
        int seq_num = received_json["seq_num"]; // Extract sequence number
        std::string data = received_json["data"]; // Extract message content
        // Check for duplicate packets
        if (received_packets.count(seq_num)) {
            std::cout << "[DUPLICATE] Ignored packet: " << seq_num << std::endl;
            return;
        }
        // Store received packet number
        received_packets.insert(seq_num);
        std::cout << "[RECEIVED] Packet " << seq_num << ": " << data << std::endl;

    } catch (std::exception& e) {
        std::cerr << "[ERROR] Invalid message format: " << e.what() << std::endl;
    }
}

int main() {
    try {
        boost::asio::io_context io_context;
        udp::socket socket(io_context, udp::endpoint(udp::v4(), SERVER_PORT));

        char buffer[1024];
        udp::endpoint sender_endpoint;

        std::cout << "[SERVER] UDP Server is running on port "<< SERVER_PORT << "..." << std::endl;

        while (true) {
            size_t length = socket.receive_from(boost::asio::buffer(buffer), sender_endpoint);
            std::string received_message(buffer, length);

            process_message(received_message, socket, sender_endpoint);
        }

    } catch (std::exception &e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
    }

    return 0;
}
