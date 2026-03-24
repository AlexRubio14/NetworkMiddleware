#include "SpatialGrid.h"
#include <algorithm>

namespace NetworkMiddleware::Core {

    using namespace Shared::GameplayConfig;

    void SpatialGrid::Clear() {
        for (auto& bitmask : m_teamVisibility)
            bitmask.reset();
    }

    std::pair<int, int> SpatialGrid::WorldToCell(float x, float y) const {
        const int col = std::clamp(
            static_cast<int>((x - MAP_MIN) / GRID_CELL_SIZE), 0, GRID_COLS - 1);
        const int row = std::clamp(
            static_cast<int>((y - MAP_MIN) / GRID_CELL_SIZE), 0, GRID_ROWS - 1);
        return {col, row};
    }

    int SpatialGrid::GetCellIndex(float x, float y) const {
        const auto [col, row] = WorldToCell(x, y);
        return row * GRID_COLS + col;
    }

    void SpatialGrid::MarkVision(float x, float y, uint8_t teamID) {
        if (teamID >= kMaxTeams)
            return;

        const auto [centerCol, centerRow] = WorldToCell(x, y);

        for (int dr = -VISION_CELL_RADIUS; dr <= VISION_CELL_RADIUS; ++dr) {
            for (int dc = -VISION_CELL_RADIUS; dc <= VISION_CELL_RADIUS; ++dc) {
                const int col = centerCol + dc;
                const int row = centerRow + dr;
                if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS)
                    continue;
                m_teamVisibility[teamID].set(row * GRID_COLS + col);
            }
        }
    }

    bool SpatialGrid::IsCellVisible(float x, float y, uint8_t teamID) const {
        if (teamID >= kMaxTeams)
            return false;
        return m_teamVisibility[teamID].test(GetCellIndex(x, y));
    }

}  // namespace NetworkMiddleware::Core
