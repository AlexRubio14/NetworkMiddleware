add_executable(NetServer main.cpp)

# Añadimos sfml-system porque el main.cpp usa sf::sleep
target_link_libraries(NetServer PRIVATE 
    MiddlewareCore 
    MiddlewareTransport 
    sfml-network 
    sfml-system
)
