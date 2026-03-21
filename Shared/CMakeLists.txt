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
        INetworkable.h
        Data/HeroState.h
        Gameplay/IHero.h
        Gameplay/BaseHero.cpp
        Gameplay/BaseHero.h
        Gameplay/HeroDirtyBits.h
        Data/Network/NetworkOptimizer.cpp
        Data/Network/NetworkOptimizer.h
        Data/Network/HeroSerializer.cpp
        Data/Network/HeroSerializer.h
        Gameplay/ViegoEntity.cpp
        Gameplay/ViegoEntity.h
        Network/PacketTypes.h
        Network/PacketHeader.h
        Network/PacketHeader.cpp
        Network/HandshakePackets.h
)

target_include_directories(MiddlewareShared PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Log
)

target_compile_features(MiddlewareShared PUBLIC cxx_std_20)