#pragma once
#include <cstdint>

// P-5.1: Centralised gameplay constants shared by SpatialGrid, HeroSerializer,
// and GameWorld. The "master variable" for FOW tuning is DEFAULT_VISION_RANGE —
// VISION_CELL_RADIUS is derived automatically so nothing else needs updating.

namespace NetworkMiddleware::Shared::GameplayConfig {

    inline constexpr float   MAP_MIN              = -500.0f;
    inline constexpr float   MAP_MAX              =  500.0f;
    inline constexpr float   GRID_CELL_SIZE       =  50.0f;
    inline constexpr float   DEFAULT_VISION_RANGE = 150.0f;

    inline constexpr int     GRID_COLS            =
        static_cast<int>((MAP_MAX - MAP_MIN) / GRID_CELL_SIZE);  // 20
    inline constexpr int     GRID_ROWS            = GRID_COLS;   // 20

    // Vision radius in cells — +1 margin prevents edge flicker at cell boundaries.
    inline constexpr int     VISION_CELL_RADIUS   =
        static_cast<int>(DEFAULT_VISION_RANGE / GRID_CELL_SIZE) + 1;  // 4

    inline constexpr uint8_t kMaxTeams            = 2;

}  // namespace NetworkMiddleware::Shared::GameplayConfig
