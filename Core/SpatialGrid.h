#pragma once
#include "../Shared/GameplayConstants.h"
#include <bitset>
#include <cstdint>
#include <utility>

namespace NetworkMiddleware::Core {

    // P-5.1 Spatial Hash Grid — shared Fog of War (FOW) for server-side interest management.
    //
    // Lifecycle per tick (threading contract — see P-4.4 Split-Phase):
    //   Main thread (before Phase A):
    //     1. Clear()             — reset both team visibility bitsets
    //     2. MarkVision(x,y,t)   — mark cells visible for each connected hero
    //   Worker threads (Phase A — read-only):
    //     3. IsCellVisible(x,y,t) — filter snapshot tasks per client
    //   Main thread (Phase B onwards):
    //     — grid is not touched again until next tick
    //
    // The grid is 20×20 cells covering [-500, 500] × [-500, 500].
    // Each cell is 50×50 world units. DEFAULT_VISION_RANGE = 150 → radius = 4 cells.
    class SpatialGrid {
    public:
        static constexpr int kTotalCells =
            Shared::GameplayConfig::GRID_COLS * Shared::GameplayConfig::GRID_ROWS;  // 400

        // Reset all team visibility bitsets. Must be called on the main thread.
        void Clear();

        // Mark every cell within VISION_CELL_RADIUS of (x, y) as visible for teamID.
        // Ignores out-of-range teamID or world positions (boundary-clamped).
        void MarkVision(float x, float y, uint8_t teamID);

        // Returns true if the cell containing (x, y) is visible to teamID.
        // Read-only — safe to call concurrently from Job System worker threads.
        bool IsCellVisible(float x, float y, uint8_t teamID) const;

        // Exposed for unit tests.
        int GetCellIndex(float x, float y) const;

    private:
        std::bitset<kTotalCells> m_teamVisibility[Shared::GameplayConfig::kMaxTeams];

        // Convert world (x, y) → (col, row) with boundary clamping.
        std::pair<int, int> WorldToCell(float x, float y) const;
    };

}  // namespace NetworkMiddleware::Core
