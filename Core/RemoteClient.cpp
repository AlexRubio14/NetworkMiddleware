#include "RemoteClient.h"

namespace NetworkMiddleware::Core {

    void RemoteClient::RecordBatch(
        uint16_t seq,
        const std::vector<std::pair<uint32_t, Shared::Data::HeroState>>& entities)
    {
        auto& entry   = m_history[seq % kHistorySize];
        entry.seq     = seq;
        entry.valid   = true;
        entry.entities.clear();
        for (const auto& p : entities)
            entry.entities.push_back(p);
    }

    void RemoteClient::ProcessAckedSeq(uint16_t seq) {
        const auto& entry = m_history[seq % kHistorySize];
        if (!entry.valid || entry.seq != seq) return;
        for (const auto& [entityID, state] : entry.entities)
            m_entityBaselines[entityID] = state;
    }

    const Shared::Data::HeroState* RemoteClient::GetEntityBaseline(uint32_t entityID) const {
        const auto it = m_entityBaselines.find(entityID);
        return (it != m_entityBaselines.end()) ? &it->second : nullptr;
    }

} // namespace NetworkMiddleware::Core
