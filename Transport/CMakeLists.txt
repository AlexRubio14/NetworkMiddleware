add_library(MiddlewareTransport STATIC
        SFMLTransport.cpp
        SFMLTransport.h
        TransportFactory.h
        TransportFactory.cpp
        RawUDPTransport.cpp
        RawUDPTransport.h
)

target_link_libraries(MiddlewareTransport
        PUBLIC MiddlewareShared
        PRIVATE sfml-network
)
target_include_directories(MiddlewareTransport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})