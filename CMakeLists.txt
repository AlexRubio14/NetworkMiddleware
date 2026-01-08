cmake_minimum_required(VERSION 3.20)
project(NetworkMiddleware LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Cargamos los módulos (orden de dependencia)
add_subdirectory(Shared)
add_subdirectory(Core)
add_subdirectory(Transport)
add_subdirectory(Server)