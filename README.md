### Build and Run
Please note! This project assumes a connection to a VPN with hardcoded assigned IP addresses. As such, communication between rover and base station will not work on your system without some modification.

In order to build and run the project, please do the following:

- Ensure you have the Boost System module installed and configured with your development environment. On Ubuntu-like systems, ```sudo apt install libboost-system-dev``` should do the trick. On Windows, you should follow the steps outlined here: https://www.boost.org/doc/libs/1_82_0/more/getting_started/windows.html
- Ensure you have CMake and a C++ compiler.
- Clone the repository.
- Create a build folder and run CMake (in build folder: ```cmake ..```).
- Build the project (in build folder: ```cmake --build .```).

CMake should automatically fetch other dependencies and use your build environment's C++ compiler. Run both components through the executables (```./base_station``` and ```./rover```).
