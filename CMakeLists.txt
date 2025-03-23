cmake_minimum_required(VERSION 3.8)
project(lunar-net-protocol VERSION 0.1.0 LANGUAGES C CXX)

# Find dependencies
find_package(Boost REQUIRED COMPONENTS system)


### COMMON
# Common library
add_library(common
    include/common/configs.hpp
)

target_include_directories(common PUBLIC
    ${CMAKE_SOURCE_DIR}/include/common
)


### BASE STATION
# Base station executable
add_executable(base_station
    src/base/base_main.cpp
    src/base/udp_server.cpp
)

# Set include directories for base_station
target_include_directories(base_station PRIVATE
    ${CMAKE_SOURCE_DIR}/include/base
)

# Link base_station with common library and boost
target_link_libraries(base_station PRIVATE common)
target_link_libraries(base_station PRIVATE Boost::system)

### ROVER
# Rover executable
add_executable(rover
    src/rover/udp_client.cpp
)

# Set include directories for moon_rover
target_include_directories(rover PRIVATE
    ${CMAKE_SOURCE_DIR}/include/rover
)

# Link rover with common library and Boost
target_link_libraries(rover PRIVATE common)
target_link_libraries(rover PRIVATE Boost::system)
