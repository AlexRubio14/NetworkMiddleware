#include <iostream>
#include <cassert>

// 1. Correct paths based on our architecture
#include "../Shared/Gameplay/ViegoEntity.h"
#include "../Shared/Data/Network/HeroSerializer.h"
#include "Data/Network/NetworkOptimizer.h"


// 2. Use the correct namespaces
using namespace NetworkMiddleware::Shared;

int main() {
    std::cout << "--- Testing Viego Network Synchronization ---" << std::endl;

    // Validació d'Endianness
    if (Network::NetworkOptimizer::IsLittleEndian()) {
        std::cout << "[System] Endianness: Little-Endian (Match Windows/Linux)" << std::endl;
    } else {
        std::cout << "[System] Endianness: Big-Endian (Swap required!)" << std::endl;
    }

    // We must use the specific namespaces for the classes
    // serverViego is Gameplay::ViegoEntity
    Gameplay::ViegoEntity serverViego(101);

    serverViego.SetPosition(12.5f, -45.8f);
    serverViego.TakeDamage(150.0f);

    // 3. Ensure we use the Network namespace for writers/readers
    BitWriter writer;
    serverViego.Serialize(writer);

    std::cout << "[Server] Bits written: " << writer.GetBitCount() << std::endl;

    // Transmission simulation
    BitReader reader(writer.GetCompressedData(), writer.GetBitCount());

    Gameplay::ViegoEntity clientViego(101);

    // Verification before sync
    assert(clientViego.GetX() != serverViego.GetX());

    clientViego.Unserialize(reader);

    std::cout << "--- Synchronization Results ---" << std::endl;
    std::cout << "Position X -> Server: " << serverViego.GetX() << " | Client: " << clientViego.GetX() << std::endl;

    return 0;
}