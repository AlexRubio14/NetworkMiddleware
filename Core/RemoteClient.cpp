#include "RemoteClient.h"

namespace NetworkMiddleware::Core {

    void RemoteClient::RecordSnapshot(uint16_t seq, const Shared::Data::HeroState& state) {
        auto& entry = m_history[seq % kHistorySize];
        entry.seq   = seq;
        entry.valid = true;
        entry.state = state;
    }

    const Shared::Data::HeroState* RemoteClient::GetBaseline(uint16_t seq) const {
        const auto& entry = m_history[seq % kHistorySize];
        if (!entry.valid || entry.seq != seq) return nullptr;
        return &entry.state;
    }

} // namespace NetworkMiddleware::Core
