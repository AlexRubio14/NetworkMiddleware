include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)
# Prevent overriding parent project compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(MiddlewareTests
    Shared/BitWriterReaderTests.cpp
    Shared/PacketHeaderTests.cpp
    Shared/SequenceContextTests.cpp
    Shared/NetworkOptimizerTests.cpp
    Shared/HeroSerializerTests.cpp
    Core/NetworkManagerTests.cpp
)

target_link_libraries(MiddlewareTests PRIVATE
    GTest::gtest_main
    MiddlewareCore
    MiddlewareShared
)

target_include_directories(MiddlewareTests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Coverage instrumentation (GCC/Clang on Linux only)
if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(MiddlewareTests  PRIVATE --coverage)
    target_compile_options(MiddlewareShared PUBLIC  --coverage)
    target_compile_options(MiddlewareCore   PUBLIC  --coverage)
    target_link_options(MiddlewareTests PRIVATE --coverage)
endif()

include(GoogleTest)
gtest_discover_tests(MiddlewareTests)