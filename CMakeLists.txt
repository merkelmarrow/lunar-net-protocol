cmake_minimum_required(VERSION 3.12)
project(lunar-net-protocol VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20 REQUIRED)
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
    include/common/basic_message.hpp
    include/common/command_message.hpp
    include/common/lumen_header.hpp
    include/common/lumen_packet.hpp
    include/common/lumen_protocol.hpp
    include/common/message.hpp
    include/common/message_manager.hpp
    include/common/message_types.hpp
    include/common/reliability_manager.hpp
    include/common/status_message.hpp
    include/common/telemetry_message.hpp
    include/common/timepoint_utils.hpp
    src/common/lumen_header.cpp
    src/common/lumen_packet.cpp
    src/common/lumen_protocol.cpp
    src/common/message.cpp
    src/common/message_manager.cpp
    src/common/reliability_manager.cpp
    src/common/timepoint_utils.cpp
)

target_include_directories(common PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(common PUBLIC nlohmann_json::nlohmann_json Boost::system)

### BASE STATION
# Base station library
add_library(base_lib
    include/base/base_station.hpp
    include/base/udp_server.hpp
    src/base/base_station.cpp
    src/base/udp_server.cpp
)

target_include_directories(base_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(base_lib PUBLIC common)

# Base station executable
add_executable(base_station
    src/base/test_base_main.cpp
)

target_link_libraries(base_station PRIVATE base_lib)

### ROVER
# Rover library
add_library(rover_lib
    include/rover/rover.hpp
    include/rover/udp_client.hpp
    src/rover/rover.cpp
    src/rover/udp_client.cpp
)

target_include_directories(rover_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rover_lib PUBLIC common)

# Rover executable
add_executable(rover
    src/rover/test_rover_main.cpp
)

target_link_libraries(rover PRIVATE rover_lib)
