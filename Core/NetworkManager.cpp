#include "NetworkManager.h"
#include "../Shared/Network/HandshakePackets.h"
#include "../Shared/Log/Logger.h"
#include <format>

namespace NetworkMiddleware::Core {

    NetworkManager::NetworkManager(std::shared_ptr<Shared::ITransport> transport)
        : m_transport(std::move(transport)) {}

    void NetworkManager::SetDataCallback(OnDataReceivedCallback callback) {
        m_onDataReceived = std::move(callback);
    }

    void NetworkManager::SetClientConnectedCallback(OnClientConnectedCallback callback) {
        m_onClientConnected = std::move(callback);
    }

    // -------------------------------------------------------------------------
    // Update — tick principal del servidor
    // Modelo de threading: Update() es llamado exclusivamente desde el tick loop
    // principal (hilo único). GetEstablishedCount()/GetPendingCount() deben
    // consultarse desde el mismo hilo. Si en el futuro se introduce un hilo de
    // red separado, añadir std::mutex sobre m_pendingClients, m_establishedClients,
    // m_nextNetworkID y m_rng.
    // -------------------------------------------------------------------------
    void NetworkManager::Update() {
        // 1. Expirar handshakes sin respuesta (> 5 segundos)
        CheckTimeouts();

        // 2. Recibir un paquete del transporte
        auto buffer = std::make_shared<std::vector<uint8_t>>();
        Shared::EndPoint sender;

        if (!m_transport->Receive(*buffer, sender))
            return;

        // 3. Validar tamaño mínimo del header
        constexpr size_t kHeaderBytes = Shared::PacketHeader::kByteCount;
        if (buffer->size() < kHeaderBytes) {
            Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                std::format("Paquete descartado: {} bytes (mínimo {})", buffer->size(), kHeaderBytes));
            return;
        }

        // 4. Parsear el header de P-3.1
        Shared::BitReader reader(*buffer, buffer->size() * 8);
        const Shared::PacketHeader header = Shared::PacketHeader::Read(reader);

        Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
            std::format("PKT seq={} ack={} ack_bits={:#010x} type={:#x} flags={:#x} ts={}ms",
                header.sequence, header.ack, header.ack_bits,
                static_cast<uint32_t>(header.type),
                static_cast<uint32_t>(header.flags),
                header.timestamp));

        // 5. Máquina de estados — enrutar según tipo de paquete
        const auto type = static_cast<Shared::PacketType>(header.type);

        switch (type) {
            case Shared::PacketType::ConnectionRequest:
                HandleConnectionRequest(sender);
                break;

            case Shared::PacketType::ChallengeResponse:
                HandleChallengeResponse(reader, sender);
                break;

            default:
                // Fix CodeRabbit: un solo find() en lugar de contains() + at()
                if (auto it = m_establishedClients.find(sender); it != m_establishedClients.end()) {
                    it->second.seqContext.RecordReceived(header.sequence);
                    if (m_onDataReceived)
                        m_onDataReceived(header, reader, sender);
                } else {
                    Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                        std::format("Paquete de cliente no autenticado: {}", sender.ToString()));
                }
                break;
        }
    }

    // -------------------------------------------------------------------------
    // CheckTimeouts — elimina handshakes que llevan > 5s sin respuesta
    // -------------------------------------------------------------------------
    void NetworkManager::CheckTimeouts() {
        std::erase_if(m_pendingClients, [this](const auto& entry) {
            if (entry.second.IsTimedOut(kHandshakeTimeout)) {
                Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
                    std::format("Handshake expirado: {}", entry.first.ToString()));
                return true;
            }
            return false;
        });
    }

    // -------------------------------------------------------------------------
    // HandleConnectionRequest — paso 1 del handshake
    //
    // Fix CodeRabbit (bug crítico): el NetworkID se reserva aquí con m_nextNetworkID++.
    // Los retries del mismo cliente reutilizan el ID ya reservado (no desperdician IDs)
    // y renuevan el salt para reiniciar el timer de 5 segundos.
    // -------------------------------------------------------------------------
    void NetworkManager::HandleConnectionRequest(const Shared::EndPoint& sender) {
        // Ignorar si ya está establecido
        if (m_establishedClients.contains(sender)) {
            Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
                std::format("ConnectionRequest de cliente ya establecido: {} — ignorado", sender.ToString()));
            return;
        }

        // Rechazar si el servidor está lleno
        if (m_establishedClients.size() >= kMaxClients) {
            Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                std::format("Servidor lleno ({} clientes). Rechazando: {}", kMaxClients, sender.ToString()));
            SendHeaderOnly(Shared::PacketType::ConnectionDenied, sender);
            return;
        }

        const uint64_t salt = m_rng();

        // Si el cliente ya estaba en pendientes (retry), reutiliza su NetworkID reservado
        // y solo renueva el salt + timer. No se desperdicia ningún ID.
        if (auto it = m_pendingClients.find(sender); it != m_pendingClients.end()) {
            it->second.challengeSalt   = salt;
            it->second.challengeSentAt = std::chrono::steady_clock::now();
            Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
                std::format("ConnectionRequest retry de {} (NetworkID={} reservado). Renovando Challenge.",
                    sender.ToString(), it->second.networkID));
            SendChallenge(sender, salt);
            return;
        }

        // Primera vez: reservar NetworkID incrementando el contador
        const uint16_t reservedID = m_nextNetworkID++;
        m_pendingClients.emplace(sender, RemoteClient(sender, reservedID, salt));

        Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
            std::format("ConnectionRequest de {}. NetworkID={} reservado. Enviando Challenge.",
                sender.ToString(), reservedID));

        SendChallenge(sender, salt);
    }

    // -------------------------------------------------------------------------
    // HandleChallengeResponse — paso 3 del handshake
    //
    // Fix CodeRabbit (bug crítico): usa pending.networkID (reservado en paso 1)
    // en lugar de volver a incrementar m_nextNetworkID.
    // -------------------------------------------------------------------------
    void NetworkManager::HandleChallengeResponse(Shared::BitReader& reader, const Shared::EndPoint& sender) {
        auto it = m_pendingClients.find(sender);
        if (it == m_pendingClients.end()) {
            Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                std::format("ChallengeResponse de {} sin Challenge previo — ignorado", sender.ToString()));
            return;
        }

        const auto response = Shared::ChallengeResponsePayload::Read(reader);
        const RemoteClient& pending = it->second;

        if (response.salt != pending.challengeSalt) {
            Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                std::format("Salt incorrecto de {} (esperado={:#x} recibido={:#x}). Rechazando.",
                    sender.ToString(), pending.challengeSalt, response.salt));
            SendHeaderOnly(Shared::PacketType::ConnectionDenied, sender);
            m_pendingClients.erase(it);
            return;
        }

        // Usar el NetworkID que se reservó en HandleConnectionRequest (no incrementar de nuevo)
        const uint16_t assignedID = pending.networkID;
        m_establishedClients.emplace(sender, RemoteClient(sender, assignedID, 0));
        m_pendingClients.erase(it);

        Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
            std::format("Cliente {} conectado. NetworkID={}", sender.ToString(), assignedID));

        SendConnectionAccepted(sender, assignedID);

        if (m_onClientConnected)
            m_onClientConnected(assignedID, sender);
    }

    // -------------------------------------------------------------------------
    // Helpers de envío
    // -------------------------------------------------------------------------

    void NetworkManager::SendHeaderOnly(Shared::PacketType type, const Shared::EndPoint& to) {
        Shared::BitWriter writer;
        Shared::PacketHeader header;
        header.type = static_cast<uint8_t>(type);
        header.Write(writer);
        m_transport->Send(writer.GetCompressedData(), to);
    }

    void NetworkManager::SendChallenge(const Shared::EndPoint& to, uint64_t salt) {
        Shared::BitWriter writer;
        Shared::PacketHeader header;
        header.type = static_cast<uint8_t>(Shared::PacketType::ConnectionChallenge);
        header.Write(writer);
        Shared::ChallengePayload{salt}.Write(writer);
        m_transport->Send(writer.GetCompressedData(), to);
    }

    void NetworkManager::SendConnectionAccepted(const Shared::EndPoint& to, uint16_t networkID) {
        Shared::BitWriter writer;
        Shared::PacketHeader header;
        header.type = static_cast<uint8_t>(Shared::PacketType::ConnectionAccepted);
        header.Write(writer);
        Shared::ConnectionAcceptedPayload{networkID}.Write(writer);
        m_transport->Send(writer.GetCompressedData(), to);
    }

}
