#include "NetworkManager.h"
#include "../Shared/Log/Logger.h"
#include <format>

namespace NetworkMiddleware::Core {

    NetworkManager::NetworkManager(std::shared_ptr<Shared::ITransport> transport)
        : m_transport(std::move(transport)) {}

    void NetworkManager::SetDataCallback(OnDataReceivedCallback callback) {
        m_onDataReceived = std::move(callback);
    }

    void NetworkManager::Update() {
        auto buffer = std::make_shared<std::vector<uint8_t>>();
        Shared::EndPoint sender;

        if (!m_transport->Receive(*buffer, sender))
            return;

        // Descartamos paquetes demasiado cortos para contener un header válido.
        // kBitCount = 104 bits = 13 bytes.
        constexpr size_t kHeaderBytes = Shared::PacketHeader::kByteCount;
        if (buffer->size() < kHeaderBytes) {
            Shared::Logger::Log(
                Shared::LogLevel::Warning,
                Shared::LogChannel::Core,
                std::format("Paquete descartado: {} bytes (mínimo {})", buffer->size(), kHeaderBytes)
            );
            return;
        }

        // Creamos un BitReader para el paquete completo.
        // BitReader avanza internamente: tras Read(), queda en el bit 104 (payload).
        Shared::BitReader reader(*buffer, buffer->size() * 8);
        const Shared::PacketHeader header = Shared::PacketHeader::Read(reader);

        Shared::Logger::Log(
            Shared::LogLevel::Info,
            Shared::LogChannel::Core,
            std::format(
                "PKT seq={} ack={} ack_bits={:#010x} type={} flags={:#x} ts={}ms",
                header.sequence, header.ack, header.ack_bits,
                static_cast<uint32_t>(header.type),
                static_cast<uint32_t>(header.flags),
                header.timestamp
            )
        );

        // Pasamos al callback: header ya parseado + reader listo para el payload.
        if (m_onDataReceived)
            m_onDataReceived(header, reader, sender);
    }

}
