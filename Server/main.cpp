#include "../Core/PacketManager.h"
#include <SFML/Network.hpp>
#include <iostream>

int main() {
    Middleware::PacketManager core;
    core.Initialize();
    std::cout << "[Server] Listo" << std::endl;
    return 0;
}
