// FowScalabilityTests.cpp — P-5.1 / P-6.3 FOW interest management at scale.
//
// Validates that SpatialGrid + VisibilityTracker correctly filter entities
// based on distance in a realistic MOBA scenario with 50 entities.
//
// Grid constants (GameplayConstants.h):
//   MAP range : [-500, 500]    GRID_CELL_SIZE: 50
//   GRID      : 20×20 = 400 cells
//   VISION_CELL_RADIUS: 4 cells  (DEFAULT_VISION_RANGE=150 → floor(150/50)+1)
//
// Observer at (0,0) → cell (10,10) → sees cells col/row ∈ [6,14]
//   Visible world range: x ∈ [-200, 250), y ∈ [-200, 250)
//
// Key design choices for positions:
//   "Near"  (radius  80): all entities land in cells [8..11] — fully inside [6,14]
//   "Far"   (radius 400): no entity lands in cells [6..14] for both axes (proved below)
//
// Proof that radius-400 ring is fully outside observer at (0,0):
//   Visible requires col ∈ [6,14] AND row ∈ [6,14].
//   col ∈ [6,14] ⟺ x ∈ [-200, 250)  ⟺  cos(θ) ∈ [-0.5, 0.625)  for r=400
//   row ∈ [6,14] ⟺ y ∈ [-200, 250)  ⟺  sin(θ) ∈ [-0.5, 0.625)
//   The two angle sets have empty intersection → zero entities visible.

#include <gtest/gtest.h>
#include "../../Core/SpatialGrid.h"
#include "../../Core/VisibilityTracker.h"
#include "../../Shared/GameplayConstants.h"

#include <cmath>
#include <vector>
#include <unordered_set>

using namespace NetworkMiddleware::Core;
using namespace NetworkMiddleware::Shared::GameplayConfig;

namespace {

struct Entity {
    uint32_t id;
    float    x, y;
};

// Place `count` entities evenly on a circle of `radius` around (cx, cy).
std::vector<Entity> MakeRing(uint32_t startID, int count,
                              float cx, float cy, float radius) {
    std::vector<Entity> out;
    out.reserve(static_cast<size_t>(count));
    const float step = 2.0f * 3.14159265f / static_cast<float>(count);
    for (int i = 0; i < count; ++i) {
        const float angle = step * static_cast<float>(i);
        out.push_back({startID + static_cast<uint32_t>(i),
                       cx + radius * std::cos(angle),
                       cy + radius * std::sin(angle)});
    }
    return out;
}

// Return entity IDs visible to `team` on the current grid.
std::vector<uint32_t> GatherVisible(const SpatialGrid&          grid,
                                     const std::vector<Entity>&  entities,
                                     uint8_t                     team) {
    std::vector<uint32_t> result;
    for (const auto& e : entities)
        if (grid.IsCellVisible(e.x, e.y, team))
            result.push_back(e.id);
    return result;
}

} // anonymous namespace

// ─── Near cluster — all 25 visible ───────────────────────────────────────────

// 25 entities on a ring of radius 80 around the observer.
// Max |x| = 80 → col = floor((80+500)/50) = 11 ∈ [6,14]  ✓
// Min  x  = -80 → col = floor((420)/50)   = 8  ∈ [6,14]  ✓  (same for y/row)
TEST(FOWWithManyEntities, NearCluster_25Entities_AllVisible) {
    SpatialGrid grid;
    grid.MarkVision(0.0f, 0.0f, 0);

    const auto near = MakeRing(1, 25, 0.0f, 0.0f, 80.0f);
    const auto visible = GatherVisible(grid, near, 0);

    EXPECT_EQ(static_cast<int>(visible.size()), 25);
}

// ─── Far ring — all 25 filtered ──────────────────────────────────────────────

// 25 entities on a ring of radius 400 around the observer.
// Proved in file header: no entity on this ring falls in cells [6,14]×[6,14].
TEST(FOWWithManyEntities, FarRing_25Entities_AllFiltered) {
    SpatialGrid grid;
    grid.MarkVision(0.0f, 0.0f, 0);

    const auto far = MakeRing(100, 25, 0.0f, 0.0f, 400.0f);
    const auto visible = GatherVisible(grid, far, 0);

    EXPECT_EQ(static_cast<int>(visible.size()), 0);
}

// ─── Mixed distance — exact 25 / 25 split ────────────────────────────────────

TEST(FOWWithManyEntities, Mixed50Entities_25Visible_25Filtered) {
    SpatialGrid grid;
    grid.MarkVision(0.0f, 0.0f, 0);

    auto all = MakeRing(1,   25, 0.0f, 0.0f,  80.0f);  // near
    auto far = MakeRing(100, 25, 0.0f, 0.0f, 400.0f);  // far
    all.insert(all.end(), far.begin(), far.end());       // 50 total

    const auto visible = GatherVisible(grid, all, 0);
    EXPECT_EQ(static_cast<int>(visible.size()), 25);

    // Visible IDs must all come from the near group (IDs 1–25)
    for (uint32_t id : visible)
        EXPECT_LE(id, 25u);
}

// ─── Second observer extends coverage ────────────────────────────────────────

// A second team-0 hero at (350, 0) adds new coverage.
// Entities near (350, 0) that were invisible to the first observer become visible.
//
// Observer at (350,0): col = floor(850/50) = 17, row = 10
//   sees cols [13,19→19], rows [6,14] → world x ∈ [150, 500), y ∈ [-200, 250)
//
// Extra ring at radius 80 around (350,0): all land in cols [15..17] ∈ visible ✓
TEST(FOWWithManyEntities, SecondObserver_ExtendsVisibility) {
    SpatialGrid grid;
    grid.MarkVision(0.0f, 0.0f, 0);

    // Without second observer: entities near (350, 0) are filtered
    const auto extra = MakeRing(200, 25, 350.0f, 0.0f, 80.0f);
    const int beforeCount = static_cast<int>(GatherVisible(grid, extra, 0).size());
    EXPECT_EQ(beforeCount, 0);

    // Add second observer
    grid.MarkVision(350.0f, 0.0f, 0);

    const int afterCount = static_cast<int>(GatherVisible(grid, extra, 0).size());
    EXPECT_EQ(afterCount, 25);
}

// ─── Team isolation with 50 entities ─────────────────────────────────────────

// Team 0 sees the left cluster; team 1 sees the right cluster.
// Neither team sees the other's cluster.
//
// Team 0 observer at (-300, 0): col=4 → sees cols [0,8]  → x ∈ [-500, -100)
// Team 1 observer at ( 300, 0): col=16 → sees cols [12,19] → x ∈ [ 100,  500)
//
// Near(-300,0) radius 80: col ∈ [floor(120/50), floor(280/50)] = [2,5] ⊂ [0,8]  ✓
// Near( 300,0) radius 80: col ∈ [floor(720/50), floor(880/50)] = [14,17] ⊂ [12,19] ✓
TEST(FOWWithManyEntities, TeamIsolation_50Entities_CorrectVisibility) {
    SpatialGrid grid;
    grid.MarkVision(-300.0f, 0.0f, 0);
    grid.MarkVision( 300.0f, 0.0f, 1);

    const auto leftCluster  = MakeRing(1,   25, -300.0f, 0.0f, 80.0f);
    const auto rightCluster = MakeRing(100, 25,  300.0f, 0.0f, 80.0f);

    // Team 0: sees left, does not see right
    EXPECT_EQ(static_cast<int>(GatherVisible(grid, leftCluster,  0).size()), 25);
    EXPECT_EQ(static_cast<int>(GatherVisible(grid, rightCluster, 0).size()),  0);

    // Team 1: sees right, does not see left
    EXPECT_EQ(static_cast<int>(GatherVisible(grid, rightCluster, 1).size()), 25);
    EXPECT_EQ(static_cast<int>(GatherVisible(grid, leftCluster,  1).size()),  0);
}

// ─── MOBA 5v5: team observers in one quadrant, 50 entities spread ─────────────

// Simulates a realistic MOBA scenario: team 0 has 5 heroes clustered in the
// bottom-left quadrant; 50 entities are distributed across the whole map.
// FOW must filter out entities in the other three quadrants.
//
// Team 0 cluster: 5 heroes near (-300, -300)
//   Observer col = floor(200/50) = 4, row = 4 → sees cols/rows [0,8]
//   Visible world: x ∈ [-500, -100), y ∈ [-500, -100)
//
// Bottom-left entities (radius 80 around (-300,-300)): col ∈ [2,5] ⊂ [0,8] → visible
// Top-right entities  (radius 80 around ( 300, 300)): col ∈ [14,17]         → NOT visible
// Top-right extras    (radius 80 around ( 300, 300), different ring)         → NOT visible
TEST(FOWWithManyEntities, MOBA_5Observers_50Entities_MajorityFiltered) {
    SpatialGrid grid;

    // 5 team-0 heroes clustered in bottom-left quadrant
    const std::vector<std::pair<float, float>> observers = {
        {-300.0f, -300.0f},
        {-280.0f, -320.0f},
        {-320.0f, -280.0f},
        {-260.0f, -300.0f},
        {-300.0f, -260.0f}
    };
    for (const auto& [x, y] : observers)
        grid.MarkVision(x, y, 0);

    // 15 entities in bottom-left cluster → all visible to team 0
    const auto bottomLeft = MakeRing(1,  15, -300.0f, -300.0f,  80.0f);
    // 25 entities in top-right quadrant (radius 80 around 300,300) → NOT visible
    const auto topRight   = MakeRing(20, 25,  300.0f,  300.0f,  80.0f);
    // 10 more entities in top-right quadrant (radius 40 around 300,300) → NOT visible
    const auto topRight2  = MakeRing(50, 10,  300.0f,  300.0f,  40.0f);

    std::vector<Entity> all;
    all.insert(all.end(), bottomLeft.begin(), bottomLeft.end());
    all.insert(all.end(), topRight.begin(),   topRight.end());
    all.insert(all.end(), topRight2.begin(),  topRight2.end());
    ASSERT_EQ(static_cast<int>(all.size()), 50);

    const auto visible = GatherVisible(grid, all, 0);

    // All 15 bottom-left entities visible, 35 filtered
    EXPECT_EQ(static_cast<int>(visible.size()), 15);

    // Bandwidth reduction: 35 out of 50 entities saved per client tick
    const int filtered = 50 - static_cast<int>(visible.size());
    EXPECT_GE(filtered, 30) << "FOW should filter at least 30/50 entities in this scenario";
}

// ─── VisibilityTracker: stable state, no reentrants ──────────────────────────

// When 25 entities are consistently visible across ticks, the tracker
// must emit zero reentrants — no unnecessary delta baseline evictions.
TEST(FOWWithManyEntities, VisibilityTracker_25StableEntities_NoReentrantsAfterPrime) {
    VisibilityTracker tracker;

    std::vector<uint32_t> visible;
    for (uint32_t id = 1; id <= 25; ++id)
        visible.push_back(id);

    tracker.UpdateAndGetReentrants(1, visible);  // prime

    // Ticks 2 and 3: same visibility → no reentrants
    EXPECT_TRUE(tracker.UpdateAndGetReentrants(1, visible).empty());
    EXPECT_TRUE(tracker.UpdateAndGetReentrants(1, visible).empty());
}

// ─── VisibilityTracker: mass exit then mass re-entry ─────────────────────────

// All 25 near entities leave the observer's FOW (observer moves away),
// then return. On re-entry the tracker must flag all 25 for baseline eviction.
TEST(FOWWithManyEntities, VisibilityTracker_25EntitiesLeaveAndReturn_AllDetectedOnReentry) {
    VisibilityTracker tracker;

    std::vector<uint32_t> near;
    for (uint32_t id = 1; id <= 25; ++id)
        near.push_back(id);

    // Tick 1: all 25 visible — prime
    tracker.UpdateAndGetReentrants(1, near);

    // Tick 2: observer moves — FOW now shows nothing
    tracker.UpdateAndGetReentrants(1, {});

    // Tick 3: observer returns — all 25 re-enter
    const auto reentrants = tracker.UpdateAndGetReentrants(1, near);

    EXPECT_EQ(static_cast<int>(reentrants.size()), 25);
    for (uint32_t id = 1; id <= 25; ++id)
        EXPECT_TRUE(reentrants.count(id)) << "Entity " << id << " not detected on re-entry";
}

// ─── VisibilityTracker: partial exit / partial re-entry ──────────────────────

// 10 of the 25 entities leave FOW, then 5 come back.
// Only those 5 should be flagged as reentrants; the remaining 15 (always
// visible) and the other 5 (still outside) should not be flagged.
TEST(FOWWithManyEntities, VisibilityTracker_PartialExitAndReentry_OnlyReentrantsDetected) {
    VisibilityTracker tracker;

    std::vector<uint32_t> all25;
    for (uint32_t id = 1; id <= 25; ++id)
        all25.push_back(id);

    // IDs 1-15 stay visible throughout; IDs 16-25 leave
    std::vector<uint32_t> stay(all25.begin(), all25.begin() + 15);          // 1..15
    std::vector<uint32_t> leave(all25.begin() + 15, all25.end());           // 16..25
    std::vector<uint32_t> returning(leave.begin(), leave.begin() + 5);      // 16..20

    // Prime
    tracker.UpdateAndGetReentrants(1, all25);

    // IDs 16-25 leave
    tracker.UpdateAndGetReentrants(1, stay);

    // IDs 16-20 return (21-25 still outside)
    std::vector<uint32_t> nowVisible = stay;
    nowVisible.insert(nowVisible.end(), returning.begin(), returning.end());
    const auto reentrants = tracker.UpdateAndGetReentrants(1, nowVisible);

    ASSERT_EQ(static_cast<int>(reentrants.size()), 5);
    for (uint32_t id = 16; id <= 20; ++id)
        EXPECT_TRUE(reentrants.count(id)) << "Entity " << id << " should be a reentrant";
    for (uint32_t id = 1; id <= 15; ++id)
        EXPECT_FALSE(reentrants.count(id)) << "Entity " << id << " was always visible, not a reentrant";
    for (uint32_t id = 21; id <= 25; ++id)
        EXPECT_FALSE(reentrants.count(id)) << "Entity " << id << " is still outside FOW";
}

// ─── VisibilityTracker: 5 independent clients, 50 entities ───────────────────

// Each client observes a different subset of the 50 entities (like different
// hero positions in a MOBA). Their visibility state must remain isolated.
TEST(FOWWithManyEntities, VisibilityTracker_5Clients_IndependentState) {
    VisibilityTracker tracker;

    // Client k sees entities in range [k*10+1 .. k*10+10]
    for (uint32_t client = 1; client <= 5; ++client) {
        std::vector<uint32_t> clientVisible;
        for (uint32_t id = (client - 1) * 10 + 1; id <= client * 10; ++id)
            clientVisible.push_back(id);
        tracker.UpdateAndGetReentrants(client, clientVisible);  // prime
    }

    // Second tick: same subsets — zero reentrants per client
    for (uint32_t client = 1; client <= 5; ++client) {
        std::vector<uint32_t> clientVisible;
        for (uint32_t id = (client - 1) * 10 + 1; id <= client * 10; ++id)
            clientVisible.push_back(id);
        EXPECT_TRUE(tracker.UpdateAndGetReentrants(client, clientVisible).empty())
            << "Client " << client << " should have no reentrants";
    }

    // Entity 5 (visible to client 1) leaves client 1's FOW, then returns
    {
        std::vector<uint32_t> c1Without5 = {1, 2, 3, 4, 6, 7, 8, 9, 10};
        tracker.UpdateAndGetReentrants(1, c1Without5);

        std::vector<uint32_t> c1With5 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        const auto reentrants = tracker.UpdateAndGetReentrants(1, c1With5);

        EXPECT_EQ(static_cast<int>(reentrants.size()), 1);
        EXPECT_TRUE(reentrants.count(5));
    }

    // Client 2 state must be unaffected
    {
        std::vector<uint32_t> c2 = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
        EXPECT_TRUE(tracker.UpdateAndGetReentrants(2, c2).empty());
    }
}
