#include <gtest/gtest.h>
#include "Core/VisibilityTracker.h"

using namespace NetworkMiddleware::Core;

// ─── First call (no prior state) ─────────────────────────────────────────────

TEST(VisibilityTracker, FirstCall_AllEntitiesAreReentrants) {
    VisibilityTracker tracker;
    const auto reentrants = tracker.UpdateAndGetReentrants(1, {10, 20, 30});
    EXPECT_EQ(reentrants.size(), 3u);
    EXPECT_TRUE(reentrants.count(10));
    EXPECT_TRUE(reentrants.count(20));
    EXPECT_TRUE(reentrants.count(30));
}

TEST(VisibilityTracker, FirstCall_EmptyVisible_ReturnsEmpty) {
    VisibilityTracker tracker;
    const auto reentrants = tracker.UpdateAndGetReentrants(1, {});
    EXPECT_TRUE(reentrants.empty());
}

// ─── Stable visibility ────────────────────────────────────────────────────────

TEST(VisibilityTracker, StableVisibility_NoReentrants) {
    VisibilityTracker tracker;
    tracker.UpdateAndGetReentrants(1, {10, 20});  // prime state
    const auto reentrants = tracker.UpdateAndGetReentrants(1, {10, 20});
    EXPECT_TRUE(reentrants.empty());
}

TEST(VisibilityTracker, StableVisibility_OverMultipleCalls) {
    VisibilityTracker tracker;
    tracker.UpdateAndGetReentrants(1, {5});
    tracker.UpdateAndGetReentrants(1, {5});
    const auto reentrants = tracker.UpdateAndGetReentrants(1, {5});
    EXPECT_TRUE(reentrants.empty());
}

// ─── New entity enters visibility ────────────────────────────────────────────

TEST(VisibilityTracker, NewEntityDetected) {
    VisibilityTracker tracker;
    tracker.UpdateAndGetReentrants(1, {10});      // prime: only entity 10
    const auto reentrants = tracker.UpdateAndGetReentrants(1, {10, 20});
    EXPECT_EQ(reentrants.size(), 1u);
    EXPECT_TRUE(reentrants.count(20));
    EXPECT_FALSE(reentrants.count(10));  // 10 was already visible
}

// ─── Entity leaves then re-enters ────────────────────────────────────────────

TEST(VisibilityTracker, EntityLeavesAndReturns_DetectedOnReentry) {
    VisibilityTracker tracker;
    tracker.UpdateAndGetReentrants(1, {10, 20});  // tick A: both visible
    tracker.UpdateAndGetReentrants(1, {10});       // tick B: 20 left FOW
    const auto reentrants = tracker.UpdateAndGetReentrants(1, {10, 20});  // tick C: 20 re-enters
    EXPECT_EQ(reentrants.size(), 1u);
    EXPECT_TRUE(reentrants.count(20));
}

TEST(VisibilityTracker, EntityLeavesAndReturns_NotDetectedIfStillVisible) {
    VisibilityTracker tracker;
    tracker.UpdateAndGetReentrants(1, {10, 20});
    tracker.UpdateAndGetReentrants(1, {10, 20});  // both still visible
    const auto reentrants = tracker.UpdateAndGetReentrants(1, {10, 20});
    EXPECT_TRUE(reentrants.empty());
}

// ─── Multiple clients — isolated state ───────────────────────────────────────

TEST(VisibilityTracker, MultipleClients_StateIsIsolated) {
    VisibilityTracker tracker;
    tracker.UpdateAndGetReentrants(1, {10});
    tracker.UpdateAndGetReentrants(2, {20});

    // Client 1 sees entity 10 again — no reentrant
    const auto r1 = tracker.UpdateAndGetReentrants(1, {10});
    EXPECT_TRUE(r1.empty());

    // Client 2 sees entity 20 again — no reentrant
    const auto r2 = tracker.UpdateAndGetReentrants(2, {20});
    EXPECT_TRUE(r2.empty());
}

TEST(VisibilityTracker, MultipleClients_IndependentReentry) {
    VisibilityTracker tracker;
    tracker.UpdateAndGetReentrants(1, {10, 20});
    tracker.UpdateAndGetReentrants(2, {10, 20});

    // Entity 20 leaves client 1's FOW, stays in client 2's
    tracker.UpdateAndGetReentrants(1, {10});
    tracker.UpdateAndGetReentrants(2, {10, 20});

    // Entity 20 re-enters client 1's FOW — should be detected for client 1 only
    const auto r1 = tracker.UpdateAndGetReentrants(1, {10, 20});
    const auto r2 = tracker.UpdateAndGetReentrants(2, {10, 20});

    EXPECT_TRUE(r1.count(20));    // re-entry for client 1
    EXPECT_FALSE(r2.count(20));   // was always visible to client 2
}

// ─── RemoveClient ─────────────────────────────────────────────────────────────

TEST(VisibilityTracker, RemoveClient_NextCallTreatsAllAsReentrants) {
    VisibilityTracker tracker;
    tracker.UpdateAndGetReentrants(1, {10, 20});  // prime
    tracker.RemoveClient(1);
    const auto reentrants = tracker.UpdateAndGetReentrants(1, {10, 20});
    EXPECT_EQ(reentrants.size(), 2u);  // fresh start after remove
}

TEST(VisibilityTracker, RemoveClient_OtherClientsUnaffected) {
    VisibilityTracker tracker;
    tracker.UpdateAndGetReentrants(1, {10});
    tracker.UpdateAndGetReentrants(2, {20});
    tracker.RemoveClient(1);
    const auto r2 = tracker.UpdateAndGetReentrants(2, {20});
    EXPECT_TRUE(r2.empty());  // client 2 unaffected
}

// ─── Clear ────────────────────────────────────────────────────────────────────

TEST(VisibilityTracker, Clear_AllClientsReset) {
    VisibilityTracker tracker;
    tracker.UpdateAndGetReentrants(1, {10});
    tracker.UpdateAndGetReentrants(2, {20});
    tracker.Clear();
    const auto r1 = tracker.UpdateAndGetReentrants(1, {10});
    const auto r2 = tracker.UpdateAndGetReentrants(2, {20});
    EXPECT_EQ(r1.size(), 1u);
    EXPECT_EQ(r2.size(), 1u);
}
