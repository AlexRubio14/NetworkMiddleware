add_library(MiddlewareShared INTERFACE)

target_sources(MiddlewareShared INTERFACE
        NetworkAddress.h
        ITransport.h
        TransportType.h
)

target_include_directories(MiddlewareShared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})