#include <iostream>
#include <thread>
#include <chrono>

// SHARED: Interfaces & data
#include "../Shared/TransportType.h"
#include "../Shared/ITransport.h"

// TRANSPORT
#include "../Transport/TransportFactory.h"

// CORE i BRAIN: Logic
#include "../Core/NetworkManager.h"
#include "../Brain/BrainManager.h"
#include "../Brain/NeuralProcessor.h"
#include "../Brain/BehaviorTree.h"

namespace NM = NetworkMiddleware;

int main() {

    auto transport = NM::Transport::TransportFactory::Create(NM::Shared::TransportType::SFML);

    if (!transport->Initialize(8888))
        return -1;

    NM::Core::NetworkManager networkManager(transport);

    auto brain = std::make_shared<NetworkMiddleware::Brain::BrainManager>(
        std::make_unique<NetworkMiddleware::Brain::NeuralProcessor>(),
        std::make_unique<NetworkMiddleware::Brain::BehaviorTree>()
    );

    networkManager.SetDataCallback([&](const auto& data, const auto& sender) {
        std::string decision = brain->DecideAction(data);
        std::cout << "[IA] Received by " << sender.ToString() << " | Decision: " << decision << std::endl;
    });

    while (true) {

        networkManager.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}