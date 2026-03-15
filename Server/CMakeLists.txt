add_executable(NetServer main.cpp)

target_link_libraries(NetServer PRIVATE
        Brain
        MiddlewareTransport
        MiddlewareShared
        MiddlewareCore
)
