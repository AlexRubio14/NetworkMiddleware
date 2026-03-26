#include "BotClient.h"
#include "../Shared/Network/HandshakePackets.h"
#include "../Shared/Serialization/BitWriter.h"
#include "../Shared/Serialization/BitReader.h"
#include "../Shared/Serialization/CRC32.h"
#include <vector>

namespace NetworkMiddleware::Core {

BotClient::BotClient(std::shared_ptr<Shared::ITransport> transport,
                     Shared::EndPoint serverEndpoint)
    : m_transport(std::move(transport))
    , m_serverEndpoint(serverEndpoint)
{}

// ─── Public API ──────────────────────────────────────────────────────────────

void BotClient::Connect() {
    if (m_state != State::Disconnected) return;
    m_state = State::Connecting;
    SendHeaderOnly(Shared::PacketType::ConnectionRequest);
}

void BotClient::Update() {
    std::vector<uint8_t> buffer;
    Shared::EndPoint sender;

    while (m_transport->Receive(buffer, sender)) {
        // P-4.5: Verify CRC32 trailer and discard corrupt packets.
        constexpr size_t kCRCSize = 4;
        if (buffer.size() < kCRCSize) continue;
        const size_t n = buffer.size() - kCRCSize;
        const uint32_t rxCRC =
              static_cast<uint32_t>(buffer[n + 0])
            | static_cast<uint32_t>(buffer[n + 1]) <<  8
            | static_cast<uint32_t>(buffer[n + 2]) << 16
            | static_cast<uint32_t>(buffer[n + 3]) << 24;
        if (rxCRC != Shared::ComputeCRC32(buffer.data(), n)) continue;
        buffer.resize(n);  // strip CRC trailer

        if (buffer.size() < Shared::PacketHeader::kByteCount) continue;
        Shared::BitReader reader(buffer, buffer.size() * 8);
        const auto header = Shared::PacketHeader::Read(reader);
        // Record the server's sequence so our outgoing ack/ack_bits stay accurate.
        m_seqCtx.RecordReceived(header.sequence);
        ProcessPacket(header, reader);
    }
}

void BotClient::SendInput(float dirX, float dirY, uint8_t buttons) {
    if (m_state != State::Established) return;

    Shared::BitWriter w;
    BuildHeader(Shared::PacketType::Input).Write(w);
    // P-5.3: echo the last server tickID received in a Snapshot packet so the
    // server can rewind to the correct historical position for lag compensation.
    // uint16_t truncation is intentional — the server performs wrap-safe comparison.
    Shared::InputPayload{dirX, dirY, buttons,
        static_cast<uint16_t>(m_lastServerTick)}.Write(w);
    m_seqCtx.AdvanceLocal();
    SendBytes(w.GetCompressedData());
}

void BotClient::Disconnect() {
    if (m_state == State::Disconnected) return;
    SendHeaderOnly(Shared::PacketType::Disconnect);
    m_state             = State::Disconnected;
    m_networkID         = 0;
    m_reconnectionToken = 0;
}

// ─── Private ─────────────────────────────────────────────────────────────────

void BotClient::ProcessPacket(const Shared::PacketHeader& header,
                               Shared::BitReader& reader) {
    switch (static_cast<Shared::PacketType>(header.type)) {
    case Shared::PacketType::ConnectionChallenge:
        HandleChallenge(reader);
        break;
    case Shared::PacketType::ConnectionAccepted:
        HandleConnectionAccepted(reader);
        break;
    case Shared::PacketType::ConnectionDenied:
        HandleConnectionDenied();
        break;
    case Shared::PacketType::Snapshot:
        // P-5.3: Snapshot payload starts with tickID:32 (see NetworkManager::SerializeSnapshot).
        // Storing it here lets SendInput echo the server's authoritative clock, keeping
        // clientTickID within kMaxRewindTicks of the server's current tickID.
        m_lastServerTick = reader.ReadBits(32);
        break;
    default:
        break;  // Heartbeats etc. — ignored by bot
    }
}

void BotClient::HandleChallenge(Shared::BitReader& reader) {
    if (m_state != State::Connecting) return;

    const auto payload = Shared::ChallengePayload::Read(reader);
    m_pendingSalt = payload.salt;
    m_state = State::Challenging;

    // Echo the salt back to the server.
    Shared::BitWriter w;
    BuildHeader(Shared::PacketType::ChallengeResponse).Write(w);
    Shared::ChallengeResponsePayload{m_pendingSalt}.Write(w);
    m_seqCtx.AdvanceLocal();
    SendBytes(w.GetCompressedData());
}

void BotClient::HandleConnectionAccepted(Shared::BitReader& reader) {
    if (m_state != State::Challenging) return;

    const auto payload = Shared::ConnectionAcceptedPayload::Read(reader);
    m_networkID         = payload.networkID;
    m_reconnectionToken = payload.reconnectionToken;
    m_state = State::Established;
}

void BotClient::HandleConnectionDenied() {
    m_state = State::Error;
}

Shared::PacketHeader BotClient::BuildHeader(Shared::PacketType type) const {
    Shared::PacketHeader h;
    h.sequence  = m_seqCtx.localSequence;
    h.ack       = m_seqCtx.remoteAck;
    h.ack_bits  = m_seqCtx.ackBits;
    h.type      = static_cast<uint8_t>(type);
    h.timestamp = Shared::PacketHeader::CurrentTimeMs();
    return h;
}

void BotClient::SendHeaderOnly(Shared::PacketType type) {
    Shared::BitWriter w;
    BuildHeader(type).Write(w);
    m_seqCtx.AdvanceLocal();
    SendBytes(w.GetCompressedData());
}

// P-4.5: Appends 4-byte CRC32 trailer (little-endian) before sending.
// All outgoing bot packets must use this instead of m_transport->Send() directly.
void BotClient::SendBytes(const std::vector<uint8_t>& wireBytes) {
    std::vector<uint8_t> frame = wireBytes;
    const uint32_t crc = Shared::ComputeCRC32(frame);
    frame.push_back(static_cast<uint8_t>((crc >>  0) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >>  8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));
    m_transport->Send(frame, m_serverEndpoint);
}

}  // namespace NetworkMiddleware::Core
