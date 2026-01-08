# Buscamos SFML específicamente para este módulo
find_package(SFML 2.5 COMPONENTS network REQUIRED)

# Definimos la librería de transporte
add_library(MiddlewareTransport STATIC 
    SFMLTransport.cpp 
    SFMLTransport.h
)

# Enlazamos con SFML y el Shared
target_link_libraries(MiddlewareTransport PUBLIC sfml-network MiddlewareShared)