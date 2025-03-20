cmake_minimum_required(VERSION 3.5.0)
project(lunar-net-protocol VERSION 0.1.0 LANGUAGES C CXX)

add_executable(lunar-net-protocol main.cpp)
find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json REQUIRED)

# Define the source files for the UDP server
set(SOURCES
    src/udp_server.cpp
)

# Create the UDP server executable
add_executable(udp_server ${SOURCES})

# Link Boost and JSON libraries (after defining udp_server)
target_link_libraries(udp_server PRIVATE Boost::system nlohmann_json::nlohmann_json)
