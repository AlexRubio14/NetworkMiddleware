#include "NetworkManager.h"
#include "../Shared/Network/HandshakePackets.h"
#include "../Shared/Network/InputPackets.h"
#include "../Shared/Data/Network/HeroSerializer.h"
#include "../Shared/Log/Logger.h"
#include "../Shared/Serialization/CRC32.h"
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
        const auto tickStart = std::chrono::steady_clock::now();

        // 1. Reintentar paquetes fiables no confirmados + detectar Link Loss
        ResendPendingPackets();

        // 2. Expirar handshakes sin respuesta (> 5 segundos)
        CheckTimeouts();

        // 3. Drenar TODOS los paquetes pendientes del buffer del OS (P-4.3: while, no if).
        // Con N bots a 60 Hz, el OS acumula cientos de paquetes entre ticks.
        // Un 'if' solo procesaría 1 por Update() y la cola crecería sin límite.
        std::vector<uint8_t> buffer;
        Shared::EndPoint sender;

        while (m_transport->Receive(buffer, sender)) {
            m_profiler.RecordBytesReceived(buffer.size());

            // 4. P-4.5: Verificar CRC32 trailer y descartar paquetes corruptos.
            // Wire format: [Header: 13 bytes] [Payload: N bytes] [CRC32: 4 bytes]
            constexpr size_t kCRCSize = 4;
            if (buffer.size() < kCRCSize) {
                m_profiler.IncrementCRCErrors();
                continue;
            }
            {
                const size_t n = buffer.size() - kCRCSize;
                const uint32_t rxCRC =
                      static_cast<uint32_t>(buffer[n + 0])
                    | static_cast<uint32_t>(buffer[n + 1]) <<  8
                    | static_cast<uint32_t>(buffer[n + 2]) << 16
                    | static_cast<uint32_t>(buffer[n + 3]) << 24;
                if (rxCRC != Shared::ComputeCRC32(buffer.data(), n)) {
                    m_profiler.IncrementCRCErrors();
                    Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                        std::format("CRC mismatch from {} — paquete descartado", sender.ToString()));
                    continue;
                }
                buffer.resize(n);  // strip CRC trailer; BitReader se construye después
            }

            // 5. Validar tamaño mínimo del header
            constexpr size_t kHeaderBytes = Shared::PacketHeader::kByteCount;
            if (buffer.size() >= kHeaderBytes) {
                // 5. Parsear el header de P-3.1
                Shared::BitReader reader(buffer, buffer.size() * 8);
                const Shared::PacketHeader header = Shared::PacketHeader::Read(reader);

                // 6. Máquina de estados — enrutar según tipo de paquete
                // NOTE: per-packet PKT log removed — at 50-100 bots × 60 Hz it
                // enqueues 3k-6k entries/s and dominates the server's measured load.
                const auto type = static_cast<Shared::PacketType>(header.type);

                switch (type) {
                    case Shared::PacketType::ConnectionRequest:
                        HandleConnectionRequest(sender);
                        break;

                    case Shared::PacketType::ChallengeResponse:
                        HandleChallengeResponse(reader, sender);
                        break;

                    case Shared::PacketType::ReconnectionRequest:
                        HandleReconnectionRequest(reader, sender);
                        break;

                    default:
                        // Un solo find() (CodeRabbit fix): no double-lookup con contains() + at()
                        if (auto it = m_establishedClients.find(sender); it != m_establishedClients.end()) {
                            auto& client = it->second;

                            // Registrar tiempo de llegada (P-3.6 keepalive).
                            client.lastIncomingTime = std::chrono::steady_clock::now();

                            // Graceful disconnect: proceso ANTES del zombie guard.
                            // Un cliente zombie puede desconectarse limpiamente (libera el slot).
                            if (type == Shared::PacketType::Disconnect) {
                                HandleDisconnect(sender);
                                break;
                            }

                            // Zombie: actualizar timing pero no procesar paquetes de juego.
                            // El cliente debe reconectarse via ReconnectionRequest.
                            if (client.isZombie) {
                                Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
                                    std::format("Paquete de cliente zombie ignorado: {}", sender.ToString()));
                                break;
                            }

                            // Detección de duplicados antes de actualizar el estado ACK.
                            // Usa seqContext.remoteAck + ackBits (misma aritmética modular int16_t).
                            // m_seqInitialized evita falso-positivo cuando remoteAck=0 y seq=0 en el primer paquete.
                            const bool isDuplicate = client.m_seqInitialized && [&]() -> bool {
                                const int16_t diff = static_cast<int16_t>(
                                    client.seqContext.remoteAck - header.sequence);
                                if (diff < 0)   return false;         // más nuevo que remoteAck → no es duplicado
                                if (diff == 0)  return true;          // exactamente remoteAck → duplicado
                                if (diff <= 32) return (client.seqContext.ackBits >> (diff - 1)) & 1u;
                                return true;                          // fuera de la ventana de 32 → descartar
                            }();

                            client.seqContext.RecordReceived(header.sequence);
                            client.m_seqInitialized = true;
                            ProcessAcks(client, header);

                            if (isDuplicate) {
                                Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
                                    std::format("Duplicado descartado: seq={} de {}", header.sequence, sender.ToString()));
                            } else if (type == Shared::PacketType::Reliable) {
                                HandleReliableOrdered(reader, client, sender, buffer.size() * 8, header);
                            } else {
                                // Filtro de paquetes Unreliable fuera de orden (P-3.4).
                                // Solo Snapshot, Input y Heartbeat pasan por esta barrera.
                                // Reliable y ReliableUnordered ya tienen su propio mecanismo de garantía.
                                const bool isUnreliableChannel =
                                    (type == Shared::PacketType::Snapshot  ||
                                     type == Shared::PacketType::Input      ||
                                     type == Shared::PacketType::Heartbeat);

                                if (isUnreliableChannel && client.m_lastProcessedSeqInitialized) {
                                    const int16_t diff = static_cast<int16_t>(
                                        header.sequence - client.m_lastProcessedSeq);
                                    if (diff <= 0) {
                                        Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
                                            std::format("Unreliable fuera de orden descartado: seq={} lastProcessed={} de {}",
                                                header.sequence, client.m_lastProcessedSeq, sender.ToString()));
                                        break;
                                    }
                                }

                                if (isUnreliableChannel) {
                                    client.m_lastProcessedSeq            = header.sequence;
                                    client.m_lastProcessedSeqInitialized = true;
                                }

                                // Heartbeat: keepalive only — do not deliver to the game layer.
                                if (type == Shared::PacketType::Heartbeat)
                                    break;

                                // Input: buffer for the game loop (ForEachEstablished) rather
                                // than firing the generic data callback.
                                if (type == Shared::PacketType::Input) {
                                    client.pendingInput = Shared::InputPayload::Read(reader);
                                    break;
                                }

                                if (m_onDataReceived)
                                    m_onDataReceived(header, reader, sender);
                            }

                        } else {
                            Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                                std::format("Paquete de cliente no autenticado: {}", sender.ToString()));
                        }
                        break;
                }
            } else {
                Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                    std::format("Paquete descartado: {} bytes (mínimo {})", buffer.size(), kHeaderBytes));
            }
        }

        // 7. Heartbeat keepalive + session timeout + zombie expiry.
        // Runs AFTER Receive() so a packet arriving just before the timeout threshold
        // updates lastIncomingTime before the zombie check runs.
        ProcessSessionKeepAlive(std::chrono::steady_clock::now());

        // 8. P-4.3: Record tick time and emit profiler report every 5 seconds.
        const auto tickEnd = std::chrono::steady_clock::now();
        m_profiler.RecordTick(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(tickEnd - tickStart).count()));
        m_profiler.MaybeReport(m_establishedClients.size());
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

        // Re-check capacity: multiple pending clients can complete their handshake in the same
        // tick (burst). HandleConnectionRequest only checked established size at Challenge time.
        if (m_establishedClients.size() >= kMaxClients) {
            Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                std::format("Servidor lleno al confirmar handshake ({} clientes). Rechazando: {}",
                    kMaxClients, sender.ToString()));
            SendHeaderOnly(Shared::PacketType::ConnectionDenied, sender);
            m_pendingClients.erase(it);
            return;
        }

        // Usar el NetworkID que se reservó en HandleConnectionRequest (no incrementar de nuevo)
        const uint16_t assignedID = pending.networkID;
        const uint64_t token      = m_rng();

        const auto now = std::chrono::steady_clock::now();
        RemoteClient newClient(sender, assignedID, 0);
        newClient.reconnectionToken = token;
        newClient.lastIncomingTime  = now;
        newClient.lastOutgoingTime  = now;
        // P-5.1: round-robin team assignment (0=Blue, 1=Red) based on arrival order.
        newClient.teamID = static_cast<uint8_t>(m_establishedClients.size() % 2);

        m_establishedClients.emplace(sender, std::move(newClient));
        m_pendingClients.erase(it);

        Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
            std::format("Cliente {} conectado. NetworkID={}", sender.ToString(), assignedID));

        SendConnectionAccepted(sender, assignedID, token);

        if (m_onClientConnected)
            m_onClientConnected(assignedID, sender);
    }

    // -------------------------------------------------------------------------
    // Helpers de envío
    // -------------------------------------------------------------------------

    // ── P-4.5 SendRaw ─────────────────────────────────────────────────────────
    // Single chokepoint for all outgoing sends. Appends a 4-byte CRC32 trailer
    // (little-endian) and delegates to the transport. Also owns RecordBytesSent
    // so callers don't duplicate profiler accounting.
    void NetworkManager::SendRaw(const std::vector<uint8_t>& wireBytes,
                                  const Shared::EndPoint& to) {
        std::vector<uint8_t> frame = wireBytes;
        const uint32_t crc = Shared::ComputeCRC32(frame);
        frame.push_back(static_cast<uint8_t>((crc >>  0) & 0xFF));
        frame.push_back(static_cast<uint8_t>((crc >>  8) & 0xFF));
        frame.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
        frame.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));
        m_transport->Send(frame, to);
        m_profiler.RecordBytesSent(frame.size());
    }

    void NetworkManager::SendHeaderOnly(Shared::PacketType type, const Shared::EndPoint& to) {
        Shared::BitWriter writer;
        Shared::PacketHeader header;
        header.type = static_cast<uint8_t>(type);
        header.Write(writer);
        SendRaw(writer.GetCompressedData(), to);
    }

    void NetworkManager::SendChallenge(const Shared::EndPoint& to, uint64_t salt) {
        Shared::BitWriter writer;
        Shared::PacketHeader header;
        header.type = static_cast<uint8_t>(Shared::PacketType::ConnectionChallenge);
        header.Write(writer);
        Shared::ChallengePayload{salt}.Write(writer);
        SendRaw(writer.GetCompressedData(), to);
    }

    void NetworkManager::SendConnectionAccepted(const Shared::EndPoint& to, uint16_t networkID, uint64_t token) {
        Shared::BitWriter writer;
        Shared::PacketHeader header;
        header.type = static_cast<uint8_t>(Shared::PacketType::ConnectionAccepted);
        header.Write(writer);
        Shared::ConnectionAcceptedPayload{networkID, token}.Write(writer);
        SendRaw(writer.GetCompressedData(), to);
    }

    void NetworkManager::SetClientDisconnectedCallback(OnClientDisconnectedCallback callback) {
        m_onClientDisconnected = std::move(callback);
    }

    // -------------------------------------------------------------------------
    // Send — envía un paquete al cliente `to` con la garantía de canal indicada.
    //
    // El header se construye con el estado ACK actual del cliente (piggybacking).
    // Para canales Reliable y ReliableUnordered se crea una entrada en m_reliableSents
    // con solo los bytes POST-header, para poder reconstruir el header fresco en
    // cada reintento con el ack/ack_bits más reciente.
    // -------------------------------------------------------------------------
    void NetworkManager::Send(const Shared::EndPoint& to, const std::vector<uint8_t>& payload,
                              Shared::PacketType channel) {
        auto it = m_establishedClients.find(to);
        if (it == m_establishedClients.end()) {
            Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                std::format("Send: destinatario {} no está establecido", to.ToString()));
            return;
        }

        RemoteClient& client = it->second;
        const uint16_t usedSeq = client.seqContext.localSequence;

        Shared::BitWriter writer;
        Shared::PacketHeader header;
        header.sequence = usedSeq;
        header.ack      = client.seqContext.remoteAck;
        header.ack_bits = client.seqContext.ackBits;
        header.type      = static_cast<uint8_t>(channel);
        header.flags     = 0;
        header.timestamp = Shared::PacketHeader::CurrentTimeMs();
        header.Write(writer);

        // Reliable Ordered: prepend el número de secuencia de canal (16 bits)
        if (channel == Shared::PacketType::Reliable)
            writer.WriteBits(client.m_nextOutgoingReliableSeq, 16);

        // Escribir el payload byte a byte
        for (uint8_t byte : payload)
            writer.WriteBits(byte, 8);

        const std::vector<uint8_t> compressed = writer.GetCompressedData();

        // Guardar en cola de reenvío si el canal requiere garantía de entrega
        const bool needsReliability = (channel == Shared::PacketType::Reliable ||
                                       channel == Shared::PacketType::ReliableUnordered);
        if (needsReliability) {
            PendingPacket pending;
            pending.sequence    = usedSeq;
            pending.lastSentTime = std::chrono::steady_clock::now();
            pending.retryCount  = 0;
            pending.channelType = static_cast<uint8_t>(channel);
            // Guardar solo los bytes post-header (el header se reconstruye en cada reenvío)
            pending.payload.assign(compressed.begin() + Shared::PacketHeader::kByteCount,
                                   compressed.end());
            client.m_reliableSents.emplace(usedSeq, std::move(pending));
        }

        // Registrar el instante de envío para RTT sampling (P-3.4) y keepalive (P-3.6).
        // usedSeq capturado antes de AdvanceLocal(), que es el seq escrito en el header.
        const auto sendTime = std::chrono::steady_clock::now();
        client.m_rtt.sentTimes[usedSeq] = sendTime;
        client.lastOutgoingTime          = sendTime;

        client.seqContext.AdvanceLocal();

        if (channel == Shared::PacketType::Reliable)
            ++client.m_nextOutgoingReliableSeq;

        SendRaw(compressed, to);  // P-4.5: appends CRC32 trailer + records bytes sent
    }

    // -------------------------------------------------------------------------
    // ResendPendingPackets — reenvía paquetes fiables no confirmados.
    //
    // Recorre todos los clientes establecidos. Si un paquete lleva más de
    // kResendInterval sin ACK, se reenvía reconstruyendo el header con el estado
    // ACK fresco del cliente (piggybacking). Tras kMaxRetries intentos fallidos
    // se considera Link Loss y se desconecta al cliente.
    // -------------------------------------------------------------------------
    void NetworkManager::ResendPendingPackets() {
        std::vector<Shared::EndPoint> toDisconnect;

        for (auto& [endpoint, client] : m_establishedClients) {
            bool shouldDisconnect = false;
            const auto now = std::chrono::steady_clock::now();

            // Intervalo adaptativo basado en RTT (P-3.4): RTT×1.5, mínimo 30ms.
            // Hasta tener muestras reales, rttEMA=100ms → intervalo inicial 150ms.
            const auto dynamicInterval = std::chrono::milliseconds(
                std::max(30LL, static_cast<long long>(client.m_rtt.rttEMA * 1.5f))
            );

            for (auto& [seq, pending] : client.m_reliableSents) {
                if (now - pending.lastSentTime < dynamicInterval)
                    continue;

                if (pending.retryCount >= kMaxRetries) {
                    shouldDisconnect = true;
                    break;
                }

                // Reconstruir header con ack/ack_bits frescos e IsRetransmit activado
                Shared::BitWriter writer;
                Shared::PacketHeader retransmitHeader;
                retransmitHeader.sequence = pending.sequence;
                retransmitHeader.ack      = client.seqContext.remoteAck;
                retransmitHeader.ack_bits = client.seqContext.ackBits;
                retransmitHeader.type     = pending.channelType;
                retransmitHeader.flags    = static_cast<uint8_t>(Shared::PacketFlags::IsRetransmit);
                retransmitHeader.Write(writer);

                for (uint8_t byte : pending.payload)
                    writer.WriteBits(byte, 8);

                const auto retransmitData = writer.GetCompressedData();
                SendRaw(retransmitData, endpoint);  // P-4.5: appends CRC32 trailer + records bytes sent
                m_profiler.IncrementRetransmissions();

                pending.lastSentTime = now;
                ++pending.retryCount;

                Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
                    std::format("Retransmit seq={} → {} (intento {}/{})",
                        seq, endpoint.ToString(), pending.retryCount, kMaxRetries));
            }

            if (shouldDisconnect)
                toDisconnect.push_back(endpoint);

            // Limpiar sentTimes con más de 2 segundos (P-3.4 hygiene).
            // Ejecutado una vez por Update() por cliente, no por paquete recibido.
            const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(2);
            std::erase_if(client.m_rtt.sentTimes, [&cutoff](const auto& e) {
                return e.second < cutoff;
            });
        }

        for (const auto& ep : toDisconnect)
            DisconnectClient(ep);
    }

    // -------------------------------------------------------------------------
    // ProcessAcks — elimina de m_reliableSents los paquetes confirmados por el
    // header recibido (usando el ack + ack_bits bitmask de P-3.1).
    // -------------------------------------------------------------------------
    void NetworkManager::ProcessAcks(RemoteClient& client, const Shared::PacketHeader& header) {
        // RTT sampling: el campo ack del header remoto indica cuál fue el último paquete
        // que el cliente confirmó. Si tenemos el instante de envío, calculamos el RTT bruto
        // y actualizamos el EMA (α=0.1). También derivamos el clock offset aproximado.
        const auto sentIt = client.m_rtt.sentTimes.find(header.ack);
        if (sentIt != client.m_rtt.sentTimes.end()) {
            const float rawRTT = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - sentIt->second).count();

            constexpr float kAlpha = 0.1f;
            client.m_rtt.rttEMA = kAlpha * rawRTT + (1.0f - kAlpha) * client.m_rtt.rttEMA;
            ++client.m_rtt.sampleCount;

            // clockOffset = ServerNow - (ClientTime + RTT/2).
            // Solo se actualiza si el cliente ha escrito un timestamp real (≠ 0).
            // Hasta que el cliente Unreal implemente CurrentTimeMs(), clockOffset permanece 0.
            if (header.timestamp != 0) {
                const float serverNow = static_cast<float>(Shared::PacketHeader::CurrentTimeMs());
                client.m_rtt.clockOffset =
                    serverNow - (static_cast<float>(header.timestamp) + client.m_rtt.rttEMA / 2.0f);
            }

            client.m_rtt.sentTimes.erase(sentIt);
        }

        std::erase_if(client.m_reliableSents, [&header](const auto& entry) {
            return header.IsAcked(entry.first);
        });

        // P-3.7: Track the last server seq the client has confirmed (header.ack is the
        // client's piggyback ACK for server-sent packets). Used by SendSnapshot() to
        // select the correct delta baseline.
        client.m_lastClientAckedServerSeq     = header.ack;
        client.m_lastClientAckedServerSeqValid = true;
    }

    // -------------------------------------------------------------------------
    // DisconnectClient — Link Loss: el cliente agotó kMaxRetries sin responder.
    // -------------------------------------------------------------------------
    void NetworkManager::DisconnectClient(const Shared::EndPoint& endpoint) {
        auto it = m_establishedClients.find(endpoint);
        if (it == m_establishedClients.end())
            return;

        const uint16_t networkID = it->second.networkID;

        Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
            std::format("Link Loss: cliente {} (NetworkID={}) desconectado tras {} reintentos",
                endpoint.ToString(), networkID, kMaxRetries));

        if (m_onClientDisconnected)
            m_onClientDisconnected(networkID, endpoint);

        m_establishedClients.erase(it);
    }

    // -------------------------------------------------------------------------
    // HandleReliableOrdered — recepción de canal Reliable Ordered (0x3).
    //
    // Lee el número de secuencia de canal (16 bits), luego el payload. Si el
    // paquete llega en orden, se entrega inmediatamente y se drenan los paquetes
    // en buffer. Si llega fuera de orden, se guarda para entrega posterior.
    // -------------------------------------------------------------------------
    void NetworkManager::HandleReliableOrdered(Shared::BitReader& reader, RemoteClient& client,
                                               const Shared::EndPoint& sender, size_t totalBits,
                                               const Shared::PacketHeader& header) {
        const uint16_t reliableSeq = static_cast<uint16_t>(reader.ReadBits(16));

        // Extraer el payload: totalBits - 100 (header) - 16 (reliableSeq) = totalBits - 116
        const size_t payloadBytes = (totalBits > 116) ? (totalBits - 116) / 8 : 0;
        std::vector<uint8_t> payloadData;
        payloadData.reserve(payloadBytes);
        for (size_t i = 0; i < payloadBytes; ++i)
            payloadData.push_back(static_cast<uint8_t>(reader.ReadBits(8)));

        if (reliableSeq == client.m_nextExpectedReliableSeq) {
            // En orden: entregar de inmediato con el header original (timestamp preservado)
            Shared::BitReader payloadReader(payloadData, payloadData.size() * 8);
            if (m_onDataReceived)
                m_onDataReceived(header, payloadReader, sender);
            ++client.m_nextExpectedReliableSeq;
            DeliverBufferedReliable(client, sender);
        } else if (static_cast<int16_t>(reliableSeq - client.m_nextExpectedReliableSeq) > 0) {
            // Fuera de orden: guardar con su header original para entrega posterior fiel
            client.m_reliableReceiveBuffer[reliableSeq] = BufferedPacket{header, std::move(payloadData)};
            Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
                std::format("Reliable Ordered fuera de orden: reliableSeq={} esperado={} — en buffer",
                    reliableSeq, client.m_nextExpectedReliableSeq));
        }
        // else: duplicado a nivel de reliableSeq, ignorar silenciosamente
    }

    // -------------------------------------------------------------------------
    // DeliverBufferedReliable — drena el buffer de recepción en orden.
    // -------------------------------------------------------------------------
    void NetworkManager::DeliverBufferedReliable(RemoteClient& client, const Shared::EndPoint& sender) {
        while (true) {
            auto it = client.m_reliableReceiveBuffer.find(client.m_nextExpectedReliableSeq);
            if (it == client.m_reliableReceiveBuffer.end())
                break;

            // Entregar con el header original almacenado (timestamp y demás campos preservados)
            const BufferedPacket& buffered = it->second;
            Shared::BitReader payloadReader(buffered.payload, buffered.payload.size() * 8);
            if (m_onDataReceived)
                m_onDataReceived(buffered.header, payloadReader, sender);

            client.m_reliableReceiveBuffer.erase(it);
            ++client.m_nextExpectedReliableSeq;
        }
    }

    // -------------------------------------------------------------------------
    // ProcessSessionKeepAlive — P-3.6
    //
    // Called every Update() with the current time. For each established client:
    //   • Non-zombie: if no incoming for kSessionTimeout → mark as zombie.
    //   • Non-zombie: if no outgoing for kHeartbeatInterval → send heartbeat.
    //   • Zombie: if zombie duration > kZombieDuration → remove + disconnect callback.
    // -------------------------------------------------------------------------
    void NetworkManager::ProcessSessionKeepAlive(std::chrono::steady_clock::time_point now) {
        std::vector<Shared::EndPoint> toExpire;

        for (auto& [endpoint, client] : m_establishedClients) {
            if (client.isZombie) {
                if (now - client.zombieTime > kZombieDuration)
                    toExpire.push_back(endpoint);
                continue;
            }

            if (now - client.lastIncomingTime > kSessionTimeout) {
                client.isZombie   = true;
                client.zombieTime = now;
                Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                    std::format("Sesión expirada: {} (NetworkID={}) → zombie. Ventana de reconexión: {}s",
                        endpoint.ToString(), client.networkID,
                        static_cast<int>(kZombieDuration.count())));
                continue;
            }

            if (now - client.lastOutgoingTime > kHeartbeatInterval) {
                Send(endpoint, {}, Shared::PacketType::Heartbeat);
                // Override with injected clock so deterministic tests work correctly.
                // Send() sets lastOutgoingTime to wall-clock; we overwrite it here
                // with the synthetic 'now' so the test's time model stays consistent.
                client.lastOutgoingTime = now;
            }
        }

        for (const auto& ep : toExpire) {
            auto it = m_establishedClients.find(ep);
            if (it == m_establishedClients.end()) continue;

            Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                std::format("Zombie expirado: {} (NetworkID={}) eliminado tras {}s",
                    ep.ToString(), it->second.networkID,
                    static_cast<int>(kZombieDuration.count())));

            if (m_onClientDisconnected)
                m_onClientDisconnected(it->second.networkID, ep);

            m_establishedClients.erase(it);
        }
    }

    // -------------------------------------------------------------------------
    // HandleDisconnect — P-3.6
    //
    // Graceful disconnect: the client explicitly closes the session.
    // Fires the disconnect callback and removes the client immediately.
    // -------------------------------------------------------------------------
    void NetworkManager::HandleDisconnect(const Shared::EndPoint& sender) {
        auto it = m_establishedClients.find(sender);
        if (it == m_establishedClients.end()) return;

        const uint16_t networkID = it->second.networkID;

        Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
            std::format("Graceful disconnect: {} (NetworkID={})", sender.ToString(), networkID));

        if (m_onClientDisconnected)
            m_onClientDisconnected(networkID, sender);

        m_establishedClients.erase(it);
    }

    // -------------------------------------------------------------------------
    // HandleReconnectionRequest — P-3.6
    //
    // A zombie client presents its networkID + reconnection token from a
    // potentially new endpoint. On success, the client is moved to the new
    // endpoint, un-zombied, and a ConnectionAccepted is sent with the same token.
    // On failure, ConnectionDenied is sent.
    // -------------------------------------------------------------------------
    void NetworkManager::HandleReconnectionRequest(Shared::BitReader& reader,
                                                   const Shared::EndPoint& sender) {
        const auto req = Shared::ReconnectionRequestPayload::Read(reader);

        // Find a zombie client with the matching networkID
        auto it = std::find_if(m_establishedClients.begin(), m_establishedClients.end(),
            [&](const auto& entry) {
                return entry.second.networkID == req.oldNetworkID && entry.second.isZombie;
            });

        if (it == m_establishedClients.end()) {
            Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                std::format("ReconnectionRequest: NetworkID={} no encontrado o no zombie — rechazado",
                    req.oldNetworkID));
            SendHeaderOnly(Shared::PacketType::ConnectionDenied, sender);
            return;
        }

        if (it->second.reconnectionToken != req.reconnectionToken) {
            Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                std::format("ReconnectionRequest: token incorrecto para NetworkID={} — rechazado",
                    req.oldNetworkID));
            SendHeaderOnly(Shared::PacketType::ConnectionDenied, sender);
            return;
        }

        // Guard: if the new endpoint is already occupied by ANOTHER active client,
        // reject the reconnection. We must check BEFORE erasing the zombie entry
        // to avoid losing the session if emplace would silently fail (map semantics).
        // sender == it->first means same-endpoint reconnect — the zombie entry IS
        // the slot we're about to replace, so no collision.
        if (sender != it->first && m_establishedClients.contains(sender)) {
            Shared::Logger::Log(Shared::LogLevel::Warning, Shared::LogChannel::Core,
                std::format("ReconnectionRequest: endpoint {} ya está ocupado — rechazado",
                    sender.ToString()));
            SendHeaderOnly(Shared::PacketType::ConnectionDenied, sender);
            return;
        }

        // Valid reconnection: move client to new endpoint
        RemoteClient client     = std::move(it->second);
        m_establishedClients.erase(it);

        const auto now          = std::chrono::steady_clock::now();
        client.endpoint         = sender;
        client.isZombie         = false;
        client.lastIncomingTime = now;
        client.lastOutgoingTime = now;

        const uint16_t networkID = client.networkID;
        const uint64_t token     = client.reconnectionToken;

        m_establishedClients.emplace(sender, std::move(client));

        Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
            std::format("Reconexión exitosa: NetworkID={} → {}", networkID, sender.ToString()));

        SendConnectionAccepted(sender, networkID, token);

        if (m_onClientConnected)
            m_onClientConnected(networkID, sender);
    }

    // -------------------------------------------------------------------------
    // GetClientNetworkStats — expone RTT y clock offset al módulo Brain (P-3.4).
    // -------------------------------------------------------------------------
    std::optional<ClientNetworkStats> NetworkManager::GetClientNetworkStats(
        const Shared::EndPoint& ep) const
    {
        const auto it = m_establishedClients.find(ep);
        if (it == m_establishedClients.end())
            return std::nullopt;

        const RTTContext& rtt = it->second.m_rtt;
        return ClientNetworkStats{
            .rtt         = rtt.rttEMA,
            .clockOffset = rtt.clockOffset,
            .sampleCount = rtt.sampleCount,
        };
    }

    NetworkProfiler::Snapshot NetworkManager::GetProfilerSnapshot() const {
        return m_profiler.GetSnapshot(m_establishedClients.size());
    }

    void NetworkManager::RecordEntitySnapshotsSent(size_t count) noexcept {
        m_profiler.RecordEntitySnapshotsSent(count);
    }

    void NetworkManager::RecordSnapshotBytesSent(size_t bytes) noexcept {
        m_profiler.RecordSnapshotBytesSent(bytes);
    }

    void NetworkManager::RecordFullTick(uint64_t microseconds) noexcept {
        m_profiler.RecordFullTick(microseconds);
    }

    bool NetworkManager::IsClientZombie(const Shared::EndPoint& ep) const {
        const auto it = m_establishedClients.find(ep);
        return it != m_establishedClients.end() && it->second.isZombie;
    }

    // -------------------------------------------------------------------------
    // SerializeSnapshot (private, const) — P-4.4 Split-Phase
    //
    // Pure serialization: builds the wire payload for a snapshot without
    // modifying any client state.  Safe to call from Job System worker threads
    // while the main thread is blocked on std::latch::wait(), because:
    //   • m_establishedClients is not modified during Phase A.
    //   • RemoteClient fields read here (m_history, m_lastClientAckedServerSeq)
    //     were written in previous ticks and are stable during Phase A.
    //   • BitWriter is a local object — no aliasing.
    //
    // Wire format: [tickID:32][HeroState bits — delta or full]
    // -------------------------------------------------------------------------
    std::vector<uint8_t> NetworkManager::SerializeSnapshot(
        const RemoteClient& client,
        const Shared::Data::HeroState& state,
        uint32_t tickID) const
    {
        Shared::BitWriter writer;
        writer.WriteBits(tickID, 32);

        const Shared::Data::HeroState* baseline = nullptr;
        if (client.m_lastClientAckedServerSeqValid)
            baseline = client.GetBaseline(client.m_lastClientAckedServerSeq);

        if (baseline)
            Shared::Network::HeroSerializer::SerializeDelta(state, *baseline, writer);
        else
            Shared::Network::HeroSerializer::Serialize(state, writer);

        return writer.GetCompressedData();
    }

    // -------------------------------------------------------------------------
    // SerializeSnapshotFor (public const) — P-4.4 Phase A entry point
    //
    // Public wrapper: looks up the client by endpoint and delegates to the
    // private SerializeSnapshot.  Returns {} if `to` is unknown.
    // -------------------------------------------------------------------------
    std::vector<uint8_t> NetworkManager::SerializeSnapshotFor(
        const Shared::EndPoint& to,
        const Shared::Data::HeroState& state,
        uint32_t tickID) const
    {
        const auto it = m_establishedClients.find(to);
        if (it == m_establishedClients.end())
            return {};
        return SerializeSnapshot(it->second, state, tickID);
    }

    // -------------------------------------------------------------------------
    // CommitAndSendSnapshot (public) — P-4.4 Phase B entry point
    //
    // Main-thread only.  Assumes Phase A has completed (std::latch::wait()
    // returned).  Records the snapshot in the client's history and transmits
    // the pre-built payload over the transport.
    //
    // usedSeq is captured BEFORE Send() because Send() calls AdvanceLocal(),
    // which increments localSequence.  RecordSnapshot must use the seq that
    // will appear in the packet header, not the next one.
    // -------------------------------------------------------------------------
    void NetworkManager::CommitAndSendSnapshot(
        const Shared::EndPoint& to,
        const Shared::Data::HeroState& state,
        const std::vector<uint8_t>& payload)
    {
        auto it = m_establishedClients.find(to);
        if (it == m_establishedClients.end())
            return;

        RemoteClient& client = it->second;
        const uint16_t usedSeq = client.seqContext.localSequence;
        client.RecordSnapshot(usedSeq, state);
        Send(to, payload, Shared::PacketType::Snapshot);
        // Record snapshot-only bytes for accurate Delta Efficiency (excludes control traffic).
        // Wire size = header (13) + payload + CRC trailer (4).
        m_profiler.RecordSnapshotBytesSent(
            Shared::PacketHeader::kByteCount + payload.size() + 4);
    }

    // -------------------------------------------------------------------------
    // SendSnapshot — P-3.7 Game Loop (single-threaded convenience path)
    //
    // Wraps SerializeSnapshot + CommitAndSendSnapshot for callers that do not
    // use the Split-Phase Job System path.  Existing tests remain unmodified.
    // -------------------------------------------------------------------------
    void NetworkManager::SendSnapshot(const Shared::EndPoint& to,
                                      const Shared::Data::HeroState& state,
                                      uint32_t tickID) {
        auto it = m_establishedClients.find(to);
        if (it == m_establishedClients.end())
            return;

        RemoteClient& client = it->second;
        const uint16_t usedSeq = client.seqContext.localSequence;
        const auto payload = SerializeSnapshot(client, state, tickID);
        client.RecordSnapshot(usedSeq, state);
        Send(to, payload, Shared::PacketType::Snapshot);
        m_profiler.RecordSnapshotBytesSent(
            Shared::PacketHeader::kByteCount + payload.size() + 4);
    }

    // -------------------------------------------------------------------------
    // GetClientTeamID — P-5.1 FOW interest management
    // -------------------------------------------------------------------------
    uint8_t NetworkManager::GetClientTeamID(const Shared::EndPoint& ep) const {
        const auto it = m_establishedClients.find(ep);
        return (it != m_establishedClients.end()) ? it->second.teamID : 0;
    }

    // -------------------------------------------------------------------------
    // ForEachEstablished — P-3.7 Game Loop
    //
    // Iterates all non-zombie established clients and invokes `callback` with:
    //   - networkID
    //   - endpoint
    //   - pointer to the buffered InputPayload (nullptr if no input this tick)
    //
    // After invoking the callback, pendingInput is cleared so the next call
    // (e.g., the snapshot phase) receives nullptr instead of stale input.
    // -------------------------------------------------------------------------
    void NetworkManager::ForEachEstablished(
        std::function<void(uint16_t, const Shared::EndPoint&, const Shared::InputPayload*)> callback)
    {
        // CONSTRAINT: the callback must NOT add or remove clients from m_establishedClients.
        // This method iterates the map directly; any structural modification would
        // invalidate the iterator and cause undefined behaviour.
        // Current callers in main.cpp (ApplyInput phase and Snapshot phase) are safe:
        // they only read GameWorld state and call SendSnapshot, neither of which
        // modifies m_establishedClients.
        for (auto& [endpoint, client] : m_establishedClients) {
            if (client.isZombie)
                continue;

            const Shared::InputPayload* inputPtr =
                client.pendingInput.has_value() ? &client.pendingInput.value() : nullptr;

            callback(client.networkID, endpoint, inputPtr);

            client.pendingInput.reset();
        }
    }

}
