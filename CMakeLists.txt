cmake_minimum_required(VERSION 3.5.0)
project(lunar-net-protocol VERSION 0.1.0 LANGUAGES C CXX)

# Find dependencies
find_package(Boost REQUIRED COMPONENTS system)

# Define the source files for the UDP server and main logic
set(SOURCES
    main.cpp
    src/udp_server.cpp
)

# Create a single executable
add_executable(lunar-net-protocol ${SOURCES})

# Link Boost (and other necessary dependencies)
target_link_libraries(lunar-net-protocol PRIVATE Boost::system)
