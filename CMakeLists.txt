cmake_minimum_required(VERSION 3.8)
project(lunar-net-protocol VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# Find dependencies
find_package(Boost REQUIRED COMPONENTS system)

# Enable fetching content from internet to fetch json lib
include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY "https://github.com/nlohmann/json"
    GIT_TAG "v3.11.3"
    FIND_PACKAGE_ARGS NAMES json)
FetchContent_MakeAvailable(json)


### COMMON
# Common library
add_library(common
    include/common/configs.hpp
    src/common/message.cpp
    src/common/timepoint_utils.cpp
    include/common/basic_message.hpp
    include/common/message_types.hpp
)

target_include_directories(common PUBLIC
    ${CMAKE_SOURCE_DIR}/include/common
)

target_link_libraries(common PRIVATE nlohmann_json::nlohmann_json)

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
