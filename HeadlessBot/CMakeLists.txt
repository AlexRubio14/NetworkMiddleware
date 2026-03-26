add_executable(HeadlessBot main.cpp)

find_package(Threads REQUIRED)

target_link_libraries(HeadlessBot PRIVATE
    MiddlewareCore
    MiddlewareTransport
    MiddlewareShared
    Threads::Threads
)

install(TARGETS HeadlessBot RUNTIME DESTINATION .)
