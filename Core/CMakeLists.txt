# Core/CMakeLists.txt

add_library(MiddlewareCore STATIC
        NetworkManager.h
        NetworkManager.cpp
        PacketManager.h
        PacketManager.cpp
        RemoteClient.h
)

target_link_libraries(MiddlewareCore PUBLIC MiddlewareShared)

target_include_directories(MiddlewareCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})