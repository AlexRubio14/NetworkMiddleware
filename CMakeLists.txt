
cmake_minimum_required(VERSION 3.20)
project(NetworkMiddleware LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build unit tests (skips SFML-dependent modules)" OFF)
option(SFML_STATIC_LIBRARIES "Link SFML statically (required for standalone distribution)" OFF)
# Propagate into the cache so FindSFML.cmake picks it up before find_package is called
set(SFML_STATIC_LIBRARIES ${SFML_STATIC_LIBRARIES} CACHE BOOL "" FORCE)

add_subdirectory(Shared)
add_subdirectory(Core)
add_subdirectory(Brain)   # no SFML dep — built in both test and production paths

if(NOT BUILD_TESTS)
    find_package(SFML COMPONENTS network system REQUIRED)
    add_subdirectory(Transport)
    add_subdirectory(Server)
    add_subdirectory(HeadlessBot)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()