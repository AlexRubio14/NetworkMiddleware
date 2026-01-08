find_package(SFML COMPONENTS network REQUIRED)

# Solo una vez este comando:
add_library(MiddlewareTransport STATIC 
    SFMLTransport.cpp 
    SFMLTransport.h
)

target_link_libraries(MiddlewareTransport PUBLIC sfml-network MiddlewareShared)
