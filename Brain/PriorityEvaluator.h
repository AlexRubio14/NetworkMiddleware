#pragma once
// Brain/PriorityEvaluator.h — P-5.4 Network LOD / AI Replication Prioritizer
//
// Assigns a replication tier (0-2) to each entity from the perspective of a
// specific observing client.  Brain-internal types only — no dependency on
// MiddlewareShared.  main.cpp converts HeroState → EvaluationTarget at the
// module boundary, following the same pattern as KalmanPredictor.
//
// Tier semantics:
//   Tier 0 — 100 Hz (every tick):  own hero; enemies within kTier0Radius or
//             within kCombatRadius of any opposing-team entity (in-combat proxy).
//   Tier 1 —  50 Hz (even ticks):  visible entities within kTier1Radius.
//   Tier 2 —  20 Hz (every 5th):   all other FOW-visible entities.
//
// Interest formula:
//   interest = (kBaseWeight + kCombatBonus * inCombat) / max(distance, 1.0f)
//   where inCombat = any opposing-team entity within kCombatRadius of the target.
//
// The evaluator is stateless — safe to call from any thread.

#include <cstdint>
#include <vector>

namespace NetworkMiddleware::Brain {

    struct EvaluationTarget {
        uint32_t entityID = 0;
        uint8_t  teamID   = 0;
        float    x        = 0.0f;
        float    y        = 0.0f;
    };

    struct EntityRelevance {
        uint32_t entityID = 0;
        uint8_t  tier     = 2;  // 0, 1, or 2
    };

    class PriorityEvaluator {
    public:
        // Tuning constants (all in world units / dimensionless).
        static constexpr float kBaseWeight    = 1.0f;
        static constexpr float kCombatBonus   = 4.0f;   // in-combat = 5× baseline
        static constexpr float kCombatRadius  = 200.0f; // proximity proxy for "in combat"
        static constexpr float kTier0Min      = kBaseWeight / 150.0f; // interest >= this → Tier 0
        static constexpr float kTier1Min      = kBaseWeight / 300.0f; // interest >= this → Tier 1

        // ComputeInCombat — O(N²) pre-pass, call ONCE per tick before Evaluate().
        //
        // Returns a parallel vector of booleans: inCombat[i] is true if
        // allEntities[i] has at least one opposing-team entity within kCombatRadius.
        // Factored out so main.cpp can compute it once and pass it to all N Evaluate()
        // calls instead of recomputing it per observer (was the O(N³) root cause).
        std::vector<bool> ComputeInCombat(
            const std::vector<EvaluationTarget>& allEntities) const;

        // Evaluate — O(N) per observer when inCombat flags are pre-computed.
        // Use this overload from the game loop (pre-compute inCombat once per tick).
        std::vector<EntityRelevance> Evaluate(
            uint32_t                             observerID,
            float                                observerX,
            float                                observerY,
            const std::vector<EvaluationTarget>& allEntities,
            const std::vector<bool>&             inCombat) const;

        // Evaluate — O(N²) convenience overload (calls ComputeInCombat internally).
        // Used by tests and any caller that doesn't need the pre-compute split.
        std::vector<EntityRelevance> Evaluate(
            uint32_t                             observerID,
            float                                observerX,
            float                                observerY,
            const std::vector<EvaluationTarget>& allEntities) const;

        // ShouldSend — decides whether an entity at the given tier should be sent
        // on tickID.  This is the single authoritative cadence rule used by both
        // the game loop and the cadence unit tests.
        //   Tier 0: every tick
        //   Tier 1: even ticks  (tickID % 2 == 0)
        //   Tier 2: every 5th tick (tickID % 5 == 0)
        static bool ShouldSend(uint8_t tier, uint32_t tickID);
    };

}  // namespace NetworkMiddleware::Brain
