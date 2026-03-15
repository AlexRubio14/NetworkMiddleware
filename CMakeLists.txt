
cmake_minimum_required(VERSION 3.20)
project(NetworkMiddleware LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SFML COMPONENTS network system REQUIRED)

add_subdirectory(Shared)
add_subdirectory(Core)
add_subdirectory(Transport)
add_subdirectory(Brain)
add_subdirectory(Server)