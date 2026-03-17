#include <iostream>
#include <thread>
#include <chrono>

// SHARED
#include "Log/Logger.h"
#include "Serialization/BitWriter.h"
#include "Serialization/BitReader.h"
#include "TransportType.h"

// TRANSPORT & CORE & BRAIN
#include "../Transport/TransportFactory.h"
#include "../Core/NetworkManager.h"
#include "../Brain/BrainManager.h"
#include "../Brain/NeuralProcessor.h"
#include "../Brain/BehaviorTree.h"

namespace NM = NetworkMiddleware;
using namespace NetworkMiddleware::Shared;

int main()
{
    Logger::Start();

    Logger::Log
    (
        LogLevel::Info,
        LogChannel::General,
        "Starting Middleware Server..."
    );

    // --- PHASE 2 TEST: Bit-packing Cycle ---
    BitWriter writer;
    uint32_t originalHealth = 2000;

    writer.WriteBits(originalHealth, 11);

    // Get ONLY compressed bytes (should be 2 bytes)
    auto compressedData = writer.GetCompressedData();
    auto packetData = std::make_shared<std::vector<uint8_t>>(compressedData);

    Logger::LogPacket
    (
        LogChannel::Core,
        packetData
    );

    // Read back test
    BitReader reader(compressedData);
    uint32_t decodedHealth = reader.ReadBits(11);

    if (decodedHealth == originalHealth)
    {
        Logger::Log
        (
            LogLevel::Info,
            LogChannel::General,
            "SERIALIZATION SUCCESS: Decoded 2000 correctly!"
        );
    }
    else
    {
        Logger::Log
        (
            LogLevel::Error,
            LogChannel::General,
            "SERIALIZATION ERROR: Data mismatch!"
        );
    }
    // --- END TEST ---

    auto transport = NM::Transport::TransportFactory::Create(TransportType::SFML);
    if (!transport->Initialize(8888))
    {
        return -1;
    }

    NM::Core::NetworkManager networkManager(transport);
    auto brain = std::make_shared<NM::Brain::BrainManager>
    (
        std::make_unique<NM::Brain::NeuralProcessor>(),
        std::make_unique<NM::Brain::BehaviorTree>()
    );

    networkManager.SetDataCallback([&](const auto& data, const auto& sender)
    {
        std::string decision = brain->DecideAction(data);
        std::cout << "[IA] Received by " << sender.ToString() << " | Decision: " << decision << std::endl;
    });

    while (true)
    {
        networkManager.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    Logger::Stop();
    return 0;
}