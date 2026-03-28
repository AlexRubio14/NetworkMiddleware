#pragma once
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NetworkMiddleware::Core {

    // P-6.3 — Interest Management: re-entry full-state guarantee.
    //
    // Tracks which entities were visible to each client in the most-recent
    // sent tick.  On the next send tick, UpdateAndGetReentrants() returns the
    // set of entity IDs that were *not* visible last tick but *are* visible
    // now.  The game loop uses this to evict stale delta baselines from
    // RemoteClient, ensuring the first packet after re-entry is a full state.
    //
    // Call only on send ticks (sendThisTick == true) so that "previous"
    // always refers to the last snapshot actually dispatched to each client.
    //
    // Thread safety: not thread-safe — call from the main thread only
    // (between Phase 0 gather and Phase A serialization).
    class VisibilityTracker {
    public:
        // Given the list of entity IDs currently visible to `clientID`, returns
        // the subset that was NOT visible in the previous call for this client
        // (i.e., entities that just entered the client's FOW).  Updates internal
        // state so the next call compares against today's visible set.
        std::unordered_set<uint32_t> UpdateAndGetReentrants(
            uint16_t                      clientID,
            const std::vector<uint32_t>&  nowVisibleIDs);

        // Removes all visibility state for a client (call on disconnect).
        void RemoveClient(uint16_t clientID);

        // Resets all state (e.g., on server restart).
        void Clear();

    private:
        // clientID → set of entity IDs that were visible in the last send tick.
        std::unordered_map<uint16_t, std::unordered_set<uint32_t>> m_prevVisible;
    };

} // namespace NetworkMiddleware::Core
