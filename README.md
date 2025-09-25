LUMEN Lunar Networking Protocol and Simulation

Status: Team lead of group project (4 members), completed. Also awarded the highest grade in the year group (77%).

Tech Stack: C/C++, CMake, Linux Networking (iptables, tc netem), libnetfilter_queue (Linux kernel library), UDP, custom networking protocol design, Boost.Asio, AWS (Lightsail), Wireguard, VPN relay, tmux, gotty webserver.

Moon-earth communication presents certain challenges that make TCP (the de facto networking transport layer protocol) ineffective. This is because TCP interprets the long round-trip delays and high error rates as congestion instead of corruption, unnecessarily throttling bandwidth. In this project, we were asked to build a networking protocol to handle a hypothetical scenario of earth to rover communication (and inter-rover communication), implementing all aspects of discovery, handshaking, messaging, reliability and other functionalities ourselves.

What we built

To ensure consistent channel modelling testing, I built a packet interception daemon in collaboration with another team that emulates the network characteristics of lunar communication. I rented an AWS Lightsail instance and set up a VPN relay server through a Wireguard interface. The server enabled a path for direct communication between groups, while intercepting each packet, labelling every sender and recipient as base station or rover, and applying accurate simulation parameters in kernel-space using Linux networking libraries. Since the service required zero application layer buy-in, we made it accessible to others and ended up with 7 other teams all communicating on the same network, with several hundred thousand packets sent over the server’s lifetime. For the network protocol, we built a layered, message-oriented protocol on top of UDP that abstracts the details of the protocol away from user space, while providing guaranteed in-order delivery over UDP. The stack is designed with resilience as top priority: it can detect a lost connection, store outbound messages, and periodically probe the base station to re-establish the session and transmit the queued data.

Interception Daemon

Interception and classification: Linux iptables rules were set up to divert all forwarded traffic into a Netfilter Queue (NFQUEUE). Our NetfilterQueue class wraps the Linux netfilter library to bind to the queue, receiving the raw packets and passing packets to be classified into four link types based on predefined IP ranges for “rovers” and “base stations”.

Bit errors and burst loss: Bit errors are modelled using a probabilistic function (with parameters chosen according to our research) to flip bits, carefully avoiding packet headers and zeroing the UDP checksum to prevent the kernel binning packets (to give teams the chance to test error-correction methods). For each link, a dedicated thread runs a simulation loop, toggling an atomic boolean to create periods of total packet loss. This models communication dropouts with configurable frequency and duration.

Latency and jitter: Packets are assigned a Netfilter mark after classification, which the TcNetemManager class uses to filter the packets into different Linux Traffic Control (tc) classes. Each class has a Network Emulator (netem) queuing discipline attached which applies link-specific latency and jitter ranges, offloading these asynchronous operations to the kernel.

Network Protocol

Framing and integrity: All data is encapsulated in a custom Lumen packet. Packets are delineated by STX/ETX, contain a header with fields for priority, sequence number, payload length, timestamp etc. A CRC-8 is calculated over the payload and header.

Asymmetric reliability: The base station sends ACKs, retransmits only when a NAK arrives. Rover expects ACKs in a sliding window, sends NAKs, and does timeout-based exponential backoff retransmits. Duplicates are handled by per-endpoint ACK bookkeeping. The asymmetric protocol is used to limit rover power consumption.

Layered for abstraction: All communication passes up and down a hierarchy of layers that abstract the workings of the protocol from the application layer. The application layer calls the MessageManager, which handles JSON serialisation and deserialisation of pre-defined message types. This is passed on to the LUMEN protocol layer which handles reliability before finally sending the datagram across the network. This same process works the same way but in reverse on the receiving side.

Base station application: binds a UDP server, manages the session, ACKs rover communications, and sends commands to the rover

Rover application: Registers the base endpoint, drives the handshake, and periodically sends telemetry to the base station. The rover also autonomously attempts to discover and listens for other rovers on the network, successfully sharing coordinates and status messages.

Impact

We demonstrated the design and implementation of a robust, stateful, and reliable communication protocol from first principles. The protocol demonstrated to be robust, recovering gracefully from burst errors, packet corruption, incorrect packet ordering, and full disconnections, even when channel impairments were artificially increased above realistic levels for testing purposes.


Build and Run

Please note! This project assumes a connection to a VPN with hardcoded assigned IP addresses. As such, communication between rover and base station will not work on your system without some modification.

In order to build and run the project, please do the following:

- Ensure you have the Boost System module installed and configured with your development environment. On Ubuntu-like systems, ```sudo apt install libboost-system-dev``` should do the trick. On Windows, you should follow the steps outlined here: https://www.boost.org/doc/libs/1_82_0/more/getting_started/windows.html
- Ensure you have CMake and a C++ compiler.
- Clone the repository.
- Create a build folder and run CMake (in build folder: ```cmake ..```).
- Build the project (in build folder: ```cmake --build .```).

CMake should automatically fetch other dependencies and use your build environment's C++ compiler. Run both components through the executables (```./base_station``` and ```./rover```).
