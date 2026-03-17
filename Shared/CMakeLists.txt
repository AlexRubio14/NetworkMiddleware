# Shared/CMakeLists.txt

# Create the static library
add_library(MiddlewareShared STATIC
        NetworkAddress.h
        ITransport.h
        ITransport.cpp
        TransportType.h
        Log/Logger.h
        Log/Logger.cpp
        Log/LogChannel.h
        Log/LogEntry.h
        Log/LogLevel.h
        Serialization/BitWriter.h
        Serialization/BitWriter.cpp
        Serialization/BitReader.h
        Serialization/BitReader.cpp
)

target_include_directories(MiddlewareShared PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Log
)

target_compile_features(MiddlewareShared PUBLIC cxx_std_20)