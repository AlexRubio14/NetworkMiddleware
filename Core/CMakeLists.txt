
add_library(MiddlewareCore STATIC
        NetworkManager.h
        NetworkManager.cpp
        PacketManager.h
        PacketManager.cpp
)

target_link_libraries(MiddlewareCore PRIVATE MiddlewareShared)
target_include_directories(MiddlewareCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})