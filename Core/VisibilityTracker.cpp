#include "VisibilityTracker.h"

namespace NetworkMiddleware::Core {

    std::unordered_set<uint32_t> VisibilityTracker::UpdateAndGetReentrants(
        uint16_t                     clientID,
        const std::vector<uint32_t>& nowVisibleIDs)
    {
        const auto nowSet = std::unordered_set<uint32_t>(nowVisibleIDs.begin(),
                                                          nowVisibleIDs.end());

        std::unordered_set<uint32_t> reentrants;

        auto it = m_prevVisible.find(clientID);
        if (it == m_prevVisible.end()) {
            // First call for this client: every visible entity is a "reentrant"
            // (no prior snapshot was sent, so we must force full state for all).
            reentrants = nowSet;
        } else {
            const auto& prev = it->second;
            for (const uint32_t eid : nowSet) {
                if (prev.find(eid) == prev.end())
                    reentrants.insert(eid);
            }
        }

        m_prevVisible[clientID] = std::move(nowSet);
        return reentrants;
    }

    void VisibilityTracker::RemoveClient(uint16_t clientID) {
        m_prevVisible.erase(clientID);
    }

    void VisibilityTracker::Clear() {
        m_prevVisible.clear();
    }

} // namespace NetworkMiddleware::Core
