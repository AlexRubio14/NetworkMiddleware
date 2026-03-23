# Core/CMakeLists.txt

add_library(MiddlewareCore STATIC
        NetworkManager.h
        NetworkManager.cpp
        NetworkProfiler.h
        NetworkProfiler.cpp
        RemoteClient.h
        RemoteClient.cpp
        BotClient.h
        BotClient.cpp
        GameWorld.h
        GameWorld.cpp
)

target_link_libraries(MiddlewareCore PUBLIC MiddlewareShared)

target_include_directories(MiddlewareCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})