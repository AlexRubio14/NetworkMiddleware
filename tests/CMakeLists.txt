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
    Shared/LoggerTests.cpp
    Shared/PacketHeaderTests.cpp
    Shared/SequenceContextTests.cpp
    Shared/NetworkOptimizerTests.cpp
    Shared/HeroSerializerTests.cpp
    Shared/DeltaCompressionTests.cpp
    Core/CRC32Tests.cpp
    Core/NetworkManagerTests.cpp
    Core/SessionRecoveryTests.cpp
    Core/BotIntegrationTests.cpp
    Core/ProfilerTests.cpp
    Core/GameWorldTests.cpp
    Core/JobSystemTests.cpp
    Core/SpatialGridTests.cpp
    Brain/KalmanTests.cpp
    Brain/PriorityEvaluatorTests.cpp
)

target_link_libraries(MiddlewareTests PRIVATE
    GTest::gtest_main
    MiddlewareCore
    MiddlewareShared
    Brain
)

target_include_directories(MiddlewareTests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Coverage instrumentation (GCC/Clang on Linux only)
# -fprofile-update=atomic prevents negative counter values when JobSystem's
# worker threads update gcov counters concurrently via std::function callbacks.
if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(MiddlewareTests  PRIVATE --coverage -fprofile-update=atomic)
    target_compile_options(MiddlewareShared PUBLIC  --coverage -fprofile-update=atomic)
    target_compile_options(MiddlewareCore   PUBLIC  --coverage -fprofile-update=atomic)
    target_compile_options(Brain            PUBLIC  --coverage -fprofile-update=atomic)
    target_link_options(MiddlewareTests PRIVATE --coverage)
endif()

include(GoogleTest)
gtest_discover_tests(MiddlewareTests)