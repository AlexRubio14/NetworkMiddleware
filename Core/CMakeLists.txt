# Core/CMakeLists.txt

find_package(Threads REQUIRED)

add_library(MiddlewareCore STATIC
        AsyncSendDispatcher.h
        AsyncSendDispatcher.cpp
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
        JobSystem.h
        JobSystem.cpp
        SpatialGrid.h
        SpatialGrid.cpp
        VisibilityTracker.h
        VisibilityTracker.cpp
        HitValidator.h
        HitValidator.cpp
)

target_link_libraries(MiddlewareCore PUBLIC MiddlewareShared Threads::Threads)

target_include_directories(MiddlewareCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
