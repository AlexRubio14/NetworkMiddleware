#pragma once
#include "../Shared/ITransport.h"
#include "../Shared/NetworkAddress.h"
#include "../Shared/Network/PacketHeader.h"
#include "../Shared/Network/PacketTypes.h"
#include "../Shared/Network/InputPackets.h"
#include <cstdint>
#include <memory>

// Client-side state machine for a headless bot (P-4.1/4.2).
//
// Designed to be transport-agnostic: in unit tests it is wired to MockTransport;
// in the HeadlessBot executable it is wired to SFMLTransport.
//
// State transitions:
//   Disconnected ──Connect()──► Connecting
//   Connecting   ──Challenge──► Challenging
//   Challenging  ──Accepted───► Established
//   Any          ──Denied─────► Error
//   Established  ──Disconnect()► Disconnected

namespace NetworkMiddleware::Core {

    class BotClient {
    public:
        enum class State {
            Disconnected,
            Connecting,   // ConnectionRequest sent, awaiting ConnectionChallenge
            Challenging,  // ChallengeResponse sent, awaiting ConnectionAccepted
            Established,  // Handshake complete — ready to send inputs
            Error         // ConnectionDenied or unrecoverable failure
        };

        explicit BotClient(std::shared_ptr<Shared::ITransport> transport,
                           Shared::EndPoint serverEndpoint);

        // Send a ConnectionRequest and transition to Connecting.
        // No-op if already Connecting or beyond.
        void Connect();

        // Drain all incoming packets and advance the state machine.
        // Must be called every game tick (or every test step).
        void Update();

        // Build and send an InputPayload packet (only in Established state).
        void SendInput(float dirX, float dirY, uint8_t buttons);

        // Send a graceful Disconnect and reset to Disconnected.
        void Disconnect();

        State    GetState()             const { return m_state; }
        uint16_t GetNetworkID()         const { return m_networkID; }
        uint64_t GetReconnectionToken() const { return m_reconnectionToken; }

    private:
        void ProcessPacket(const Shared::PacketHeader& header, Shared::BitReader& reader);
        void HandleChallenge(Shared::BitReader& reader);
        void HandleConnectionAccepted(Shared::BitReader& reader);
        void HandleConnectionDenied();

        // Returns a fully-stamped header (seq/ack/ack_bits/timestamp) for the given type.
        // Caller must call m_seqCtx.AdvanceLocal() after each outgoing packet.
        Shared::PacketHeader BuildHeader(Shared::PacketType type) const;

        // Helper: build a header-only packet and send it, then advance the local sequence.
        void SendHeaderOnly(Shared::PacketType type);

        // P-4.5 Packet Integrity — single chokepoint for all outgoing sends.
        // Appends a 4-byte CRC32 trailer (little-endian) before calling m_transport->Send().
        void SendBytes(const std::vector<uint8_t>& wireBytes);

        std::shared_ptr<Shared::ITransport> m_transport;
        Shared::EndPoint                    m_serverEndpoint;

        State    m_state             = State::Disconnected;
        uint16_t m_networkID         = 0;
        uint64_t m_reconnectionToken = 0;
        uint64_t m_pendingSalt       = 0;

        // P-5.3: last server tickID received in a Snapshot packet.
        // Echoed back in every InputPayload so the server can locate the correct
        // rewind slot for lag compensation.  Starts at 0; safe because abilities
        // are only pressed once the bot is Established and has received snapshots.
        uint32_t m_lastServerTick = 0;

        // Tracks outgoing sequence numbers and incoming ACK state for piggybacking.
        Shared::SequenceContext m_seqCtx;
    };

}  // namespace NetworkMiddleware::Core
