#include <iostream>
#include <memory>
#include <vector>
#include <SFML/System.hpp> // Necesario para sf::sleep
#include "../Transport/SFMLTransport.h"
#include "../Shared/ITransport.h"

int main() {
    std::cout << "[Core] Inicializado" << std::endl;

    // Instanciamos usando la interfaz
    std::unique_ptr<Middleware::ITransport> network = std::make_unique<Middleware::SFMLTransport>();

    if (!network->Initialize(8888)) {
        std::cerr << "[Error] No se pudo binear el puerto 8888" << std::endl;
        return -1;
    }

    std::cout << "[Server] Escuchando en el puerto 8888..." << std::endl;

    std::vector<uint8_t> buffer;
    std::string senderAddress;
    uint16_t senderPort;

    while (true) {
        if (network->Receive(buffer, senderAddress, senderPort)) {
            std::string mensaje(buffer.begin(), buffer.end());
            std::cout << "[Red] Recibido: " << mensaje << std::endl;
        }
        sf::sleep(sf::milliseconds(10)); // Evita saturar la CPU
    }
    return 0;
}
