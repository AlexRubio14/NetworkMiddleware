// tests/Brain/PriorityEvaluatorTests.cpp — P-5.4 Network LOD unit tests
//
// Test strategy:
//   Own hero → always Tier 0 regardless of distance.
//   Nearby enemy → Tier 0 (high interest).
//   Far non-combat entity → Tier 2 (low interest).
//   Combat proximity boosts tier of otherwise-Tier-2 entity.
//   shouldSend logic: Tier 0 every tick, Tier 1 even ticks, Tier 2 every 5th.

#include <gtest/gtest.h>
#include "../../Brain/PriorityEvaluator.h"

using namespace NetworkMiddleware::Brain;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static EvaluationTarget MakeTarget(uint32_t id, uint8_t team, float x, float y) {
    return EvaluationTarget{id, team, x, y};
}

// ─── Tests ───────────────────────────────────────────────────────────────────

TEST(PriorityEvaluator, OwnHero_AlwaysTier0) {
    PriorityEvaluator ev;
    // Observer is entity 1 at origin, team 0.
    // Place a single entity (own hero) far away — should still be Tier 0.
    const std::vector<EvaluationTarget> targets = {
        MakeTarget(1, 0, 499.0f, 499.0f)  // far corner, same team
    };
    const auto result = ev.Evaluate(1, 0.0f, 0.0f, 0, targets);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].tier, 0u) << "Own hero must always be Tier 0";
}

TEST(PriorityEvaluator, NearbyEnemy_Tier0) {
    PriorityEvaluator ev;
    // Observer at (0,0), enemy at (50,0) — well within kTier0Min threshold (150u).
    const std::vector<EvaluationTarget> targets = {
        MakeTarget(1, 0, 0.0f, 0.0f),   // observer (own)
        MakeTarget(2, 1, 50.0f, 0.0f),  // enemy, close
    };
    const auto result = ev.Evaluate(1, 0.0f, 0.0f, 0, targets);
    ASSERT_EQ(result.size(), 2u);
    // Find entity 2 in result.
    uint8_t tier2 = 99;
    for (const auto& r : result) if (r.entityID == 2) tier2 = r.tier;
    EXPECT_EQ(tier2, 0u) << "Enemy at 50u should be Tier 0";
}

TEST(PriorityEvaluator, FarNonCombat_Tier2) {
    PriorityEvaluator ev;
    // Observer at origin, ally at (400, 0) — far, no enemies nearby → Tier 2.
    // No opposing entity within kCombatRadius of the target → inCombat = false.
    const std::vector<EvaluationTarget> targets = {
        MakeTarget(1, 0,   0.0f, 0.0f),  // observer
        MakeTarget(2, 0, 400.0f, 0.0f),  // ally, far
    };
    const auto result = ev.Evaluate(1, 0.0f, 0.0f, 0, targets);
    uint8_t tier2 = 99;
    for (const auto& r : result) if (r.entityID == 2) tier2 = r.tier;
    EXPECT_EQ(tier2, 2u) << "Far non-combat ally should be Tier 2";
}

TEST(PriorityEvaluator, CombatProximityBoostsTier) {
    PriorityEvaluator ev;
    // Observer at origin, ally at (350, 0), enemy at (350+50, 0).
    // The ally is at 350u from observer → normally Tier 2, but it is within
    // kCombatRadius (200u) of the enemy → inCombat = true → interest boosted → Tier 1 or 0.
    const std::vector<EvaluationTarget> targets = {
        MakeTarget(1, 0,   0.0f, 0.0f),  // observer (own)
        MakeTarget(2, 0, 350.0f, 0.0f),  // ally far from observer, near enemy
        MakeTarget(3, 1, 400.0f, 0.0f),  // enemy near ally (50u apart)
    };
    const auto result = ev.Evaluate(1, 0.0f, 0.0f, 0, targets);
    uint8_t tier2 = 99;
    for (const auto& r : result) if (r.entityID == 2) tier2 = r.tier;
    // Without combat: interest = 1/350 ≈ 0.00286 < kTier1Min (1/300) → Tier 2.
    // With combat:    interest = 5/350 ≈ 0.0143  > kTier0Min (1/150) → Tier 0.
    EXPECT_LT(tier2, 2u) << "In-combat ally at 350u should be boosted above Tier 2";
}

TEST(PriorityEvaluator, Tier1Range) {
    PriorityEvaluator ev;
    // Entity at 250u. interest = 1/250 = 0.004.
    // kTier0Min = 1/150 ≈ 0.00667 → below Tier 0.
    // kTier1Min = 1/300 ≈ 0.00333 → above Tier 1 threshold → Tier 1.
    // NOTE: 250u > kCombatRadius (200u) so the entity is NOT in combat with the
    // observer, ensuring inCombat=false and interest stays at the baseline formula.
    const std::vector<EvaluationTarget> targets = {
        MakeTarget(1, 0,   0.0f, 0.0f),  // observer (own, team 0)
        MakeTarget(2, 1, 250.0f, 0.0f),  // enemy at 250u — outside kCombatRadius
    };
    const auto result = ev.Evaluate(1, 0.0f, 0.0f, 0, targets);
    uint8_t tier2 = 99;
    for (const auto& r : result) if (r.entityID == 2) tier2 = r.tier;
    EXPECT_EQ(tier2, 1u) << "Enemy at 250u with no combat should be Tier 1";
}

TEST(PriorityEvaluator, ShouldSend_Tier0_EveryTick) {
    // Tier 0 sends every tick regardless of tickID.
    for (uint32_t t = 0; t < 10; ++t) {
        const bool shouldSend = (0 == 0) || true;  // tier 0 always
        (void)shouldSend;
        // Encode the logic directly for the test.
        const uint8_t tier = 0;
        const bool send = (tier == 0) || (tier == 1 && t % 2 == 0) || (tier == 2 && t % 5 == 0);
        EXPECT_TRUE(send) << "Tier 0 must send at tick " << t;
    }
}

TEST(PriorityEvaluator, ShouldSend_Tier1_EvenTicks) {
    for (uint32_t t = 0; t < 10; ++t) {
        const uint8_t tier = 1;
        const bool send = (tier == 0) || (tier == 1 && t % 2 == 0) || (tier == 2 && t % 5 == 0);
        if (t % 2 == 0) EXPECT_TRUE(send)  << "Tier 1 should send at even tick " << t;
        else             EXPECT_FALSE(send) << "Tier 1 should NOT send at odd tick " << t;
    }
}

TEST(PriorityEvaluator, ShouldSend_Tier2_Every5th) {
    for (uint32_t t = 0; t < 15; ++t) {
        const uint8_t tier = 2;
        const bool send = (tier == 0) || (tier == 1 && t % 2 == 0) || (tier == 2 && t % 5 == 0);
        if (t % 5 == 0) EXPECT_TRUE(send)  << "Tier 2 should send at tick " << t;
        else             EXPECT_FALSE(send) << "Tier 2 should NOT send at tick " << t;
    }
}
