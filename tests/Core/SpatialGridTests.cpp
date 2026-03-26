#include <gtest/gtest.h>
#include "../../Core/SpatialGrid.h"
#include "../../Shared/GameplayConstants.h"

using namespace NetworkMiddleware::Core;
using namespace NetworkMiddleware::Shared::GameplayConfig;

// ─── Constants ───────────────────────────────────────────────────────────────

// Test_Grid_Abstraction: VISION_CELL_RADIUS is derived automatically from
// DEFAULT_VISION_RANGE and GRID_CELL_SIZE. Changing one variable updates the
// other without manual intervention.
TEST(SpatialGrid, Test_Grid_Abstraction) {
    // DEFAULT_VISION_RANGE = 150, GRID_CELL_SIZE = 50 → floor(150/50)+1 = 4
    EXPECT_EQ(VISION_CELL_RADIUS, 4);
    EXPECT_EQ(GRID_COLS, 20);
    EXPECT_EQ(GRID_ROWS, 20);
    EXPECT_EQ(static_cast<int>(MAP_MAX - MAP_MIN), 1000);
}

// ─── Cell index mapping ───────────────────────────────────────────────────────

TEST(SpatialGrid, GetCellIndex_Origin) {
    SpatialGrid grid;
    // (0, 0) → col=10, row=10 → index = 10*20 + 10 = 210
    EXPECT_EQ(grid.GetCellIndex(0.0f, 0.0f), 210);
}

TEST(SpatialGrid, GetCellIndex_MinCorner) {
    SpatialGrid grid;
    // (-500, -500) → col=0, row=0 → index = 0
    EXPECT_EQ(grid.GetCellIndex(-500.0f, -500.0f), 0);
}

TEST(SpatialGrid, GetCellIndex_MaxCorner) {
    SpatialGrid grid;
    // (500, 500) → clamped to col=19, row=19 → index = 19*20 + 19 = 399
    EXPECT_EQ(grid.GetCellIndex(500.0f, 500.0f), SpatialGrid::kTotalCells - 1);
}

// ─── Boundary clamping ────────────────────────────────────────────────────────

// Test_Boundary_Clamping: entities at map edge must not cause out-of-bounds
// access on the bitset. MarkVision and IsCellVisible must be safe at ±500.
TEST(SpatialGrid, Test_Boundary_Clamping) {
    SpatialGrid grid;
    // Should not crash or assert
    EXPECT_NO_FATAL_FAILURE(grid.MarkVision(500.0f, 500.0f, 0));
    EXPECT_NO_FATAL_FAILURE(grid.MarkVision(-500.0f, -500.0f, 0));
    EXPECT_TRUE(grid.IsCellVisible(500.0f, 500.0f, 0));
    EXPECT_TRUE(grid.IsCellVisible(-500.0f, -500.0f, 0));
}

TEST(SpatialGrid, BoundaryClamp_NegativeOvershoot) {
    SpatialGrid grid;
    // Positions beyond MAP_MIN / MAP_MAX are clamped — no crash, no UB
    EXPECT_NO_FATAL_FAILURE(grid.MarkVision(-600.0f, -600.0f, 1));
    EXPECT_NO_FATAL_FAILURE(grid.MarkVision(600.0f, 600.0f, 1));
}

// ─── Clear ────────────────────────────────────────────────────────────────────

TEST(SpatialGrid, Clear_ResetsAllVisibility) {
    SpatialGrid grid;
    grid.MarkVision(0.0f, 0.0f, 0);
    grid.MarkVision(0.0f, 0.0f, 1);
    grid.Clear();
    EXPECT_FALSE(grid.IsCellVisible(0.0f, 0.0f, 0));
    EXPECT_FALSE(grid.IsCellVisible(0.0f, 0.0f, 1));
}

// ─── Team isolation ───────────────────────────────────────────────────────────

TEST(SpatialGrid, TeamVisibility_IsIsolated) {
    SpatialGrid grid;
    grid.MarkVision(0.0f, 0.0f, 0);  // only team 0 marks origin
    EXPECT_TRUE(grid.IsCellVisible(0.0f, 0.0f, 0));
    EXPECT_FALSE(grid.IsCellVisible(0.0f, 0.0f, 1));  // team 1 has no vision here
}

TEST(SpatialGrid, InvalidTeamID_ReturnsFalse) {
    SpatialGrid grid;
    grid.MarkVision(0.0f, 0.0f, 99);  // silently ignored
    EXPECT_FALSE(grid.IsCellVisible(0.0f, 0.0f, 99));
}

// ─── FOW culling ─────────────────────────────────────────────────────────────

// Test_FogOfWar_Culling: enemy far outside VISION_CELL_RADIUS is not visible
// to the observing team → the server would skip its snapshot packet.
TEST(SpatialGrid, Test_FogOfWar_Culling) {
    SpatialGrid grid;

    // Team 0 hero at origin (0, 0). Vision covers roughly ±200 world units.
    grid.MarkVision(0.0f, 0.0f, 0);

    // Enemy (team 1) at (400, 400) — well outside vision range.
    // IsCellVisible with team 0 must return false → server skips the snapshot.
    EXPECT_FALSE(grid.IsCellVisible(400.0f, 400.0f, 0));

    // The team 0 hero can see its own position.
    EXPECT_TRUE(grid.IsCellVisible(0.0f, 0.0f, 0));
}

TEST(SpatialGrid, FogOfWar_EnemyJustOutsideRadius) {
    SpatialGrid grid;
    // Origin hero with VISION_CELL_RADIUS=4 covers cells col=[6,14], row=[6,14]
    grid.MarkVision(0.0f, 0.0f, 0);

    // (350, 0) → col=17 → outside the vision square
    EXPECT_FALSE(grid.IsCellVisible(350.0f, 0.0f, 0));
}

TEST(SpatialGrid, FogOfWar_EnemyJustInsideRadius) {
    SpatialGrid grid;
    // Hero at origin, radius=4 → covers up to col=14 from col=10+4=14
    grid.MarkVision(0.0f, 0.0f, 0);

    // (175, 0) → col = (175+500)/50 = 13 → within [6,14] → visible
    EXPECT_TRUE(grid.IsCellVisible(175.0f, 0.0f, 0));
}

// ─── Team shared vision (union) ───────────────────────────────────────────────

// Test_Team_Vision_Merge: if two allies mark different areas, the team bitset
// is the union — both zones are visible for that team.
TEST(SpatialGrid, Test_Team_Vision_Merge) {
    SpatialGrid grid;

    // Two team-0 heroes in opposite corners of the map
    grid.MarkVision(-250.0f, 0.0f, 0);  // left side
    grid.MarkVision( 250.0f, 0.0f, 0);  // right side

    // Each hero's immediate cell must be visible
    EXPECT_TRUE(grid.IsCellVisible(-250.0f, 0.0f, 0));
    EXPECT_TRUE(grid.IsCellVisible( 250.0f, 0.0f, 0));

    // The area far between them (origin) should NOT be visible (gap > 2 radii)
    // col(-250) = (-250+500)/50 = 5; col(250) = (250+500)/50 = 15
    // radius=4 → left covers [1,9], right covers [11,19] → origin col=10 uncovered
    EXPECT_FALSE(grid.IsCellVisible(0.0f, 0.0f, 0));

    // Team 1 sees neither zone (no heroes marked for team 1)
    EXPECT_FALSE(grid.IsCellVisible(-250.0f, 0.0f, 1));
    EXPECT_FALSE(grid.IsCellVisible( 250.0f, 0.0f, 1));
}

TEST(SpatialGrid, TeamVision_AllyOverlapMerges) {
    SpatialGrid grid;
    // Two allies near each other — overlapping vision, both zones visible
    grid.MarkVision(-25.0f, 0.0f, 0);
    grid.MarkVision( 25.0f, 0.0f, 0);

    EXPECT_TRUE(grid.IsCellVisible(-25.0f, 0.0f, 0));
    EXPECT_TRUE(grid.IsCellVisible( 25.0f, 0.0f, 0));
    EXPECT_TRUE(grid.IsCellVisible(  0.0f, 0.0f, 0));  // overlap region
}
