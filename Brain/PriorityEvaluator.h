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

        // Evaluate replication tiers for all entities from the observer's perspective.
        //
        // observerID   — networkID of the observing client (always Tier 0 for own hero).
        // observerX/Y  — observer's current world position.
        // observerTeam — observer's team ID (0 = Blue, 1 = Red).
        // allEntities  — all entities in the world (FOW filtering is done upstream).
        //
        // Returns one EntityRelevance per entry in allEntities.
        std::vector<EntityRelevance> Evaluate(
            uint32_t                          observerID,
            float                             observerX,
            float                             observerY,
            uint8_t                           observerTeam,
            const std::vector<EvaluationTarget>& allEntities) const;
    };

}  // namespace NetworkMiddleware::Brain
