# Server/CMakeLists.txt

add_executable(NetServer main.cpp)

find_package(Threads REQUIRED)

target_link_libraries(NetServer PRIVATE
        Brain
        MiddlewareCore
        MiddlewareTransport
        MiddlewareShared
        Threads::Threads
)

install(TARGETS NetServer RUNTIME DESTINATION .)