#include "BotClient.h"
#include "../Shared/Network/HandshakePackets.h"
#include "../Shared/Serialization/BitWriter.h"
#include "../Shared/Serialization/BitReader.h"
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
        if (buffer.empty()) continue;
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
    Shared::InputPayload{dirX, dirY, buttons}.Write(w);
    m_seqCtx.AdvanceLocal();
    m_transport->Send(w.GetCompressedData(), m_serverEndpoint);
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
    default:
        break;  // Heartbeats, Snapshots etc. — ignored by bot for now
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
    m_transport->Send(w.GetCompressedData(), m_serverEndpoint);
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
    m_transport->Send(w.GetCompressedData(), m_serverEndpoint);
}

}  // namespace NetworkMiddleware::Core
