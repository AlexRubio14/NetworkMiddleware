#pragma once
#include "../Shared/ITransport.h"
#include "../Shared/Network/PacketHeader.h"
#include "../Shared/Serialization/BitReader.h"
#include <memory>
#include <functional>

namespace NetworkMiddleware::Core {

    // El callback ya no recibe bytes crudos: recibe el header parseado y un
    // BitReader posicionado en el bit 104 (inicio del payload), listo para leer.
    using OnDataReceivedCallback = std::function<void(
        const Shared::PacketHeader&,
        Shared::BitReader&,
        const Shared::EndPoint&
    )>;

    class NetworkManager {
    private:
        std::shared_ptr<Shared::ITransport> m_transport;
        OnDataReceivedCallback m_onDataReceived;

    public:
        explicit NetworkManager(std::shared_ptr<Shared::ITransport> transport);

        void SetDataCallback(OnDataReceivedCallback callback);
        void Update();
    };
}
