#include "PriorityEvaluator.h"
#include <cmath>

namespace NetworkMiddleware::Brain {

std::vector<bool> PriorityEvaluator::ComputeInCombat(
    const std::vector<EvaluationTarget>& allEntities) const
{
    const size_t n = allEntities.size();
    std::vector<bool> inCombat(n, false);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            if (i == j) continue;
            if (allEntities[i].teamID == allEntities[j].teamID) continue;
            const float dx = allEntities[i].x - allEntities[j].x;
            const float dy = allEntities[i].y - allEntities[j].y;
            if ((dx * dx + dy * dy) <= (kCombatRadius * kCombatRadius)) {
                inCombat[i] = true;
                break;
            }
        }
    }
    return inCombat;
}

std::vector<EntityRelevance> PriorityEvaluator::Evaluate(
    uint32_t                             observerID,
    float                                observerX,
    float                                observerY,
    const std::vector<EvaluationTarget>& allEntities,
    const std::vector<bool>&             inCombat) const
{
    const size_t n = allEntities.size();
    std::vector<EntityRelevance> result;
    result.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        const auto& target = allEntities[i];
        EntityRelevance rel;
        rel.entityID = target.entityID;

        if (target.entityID == observerID) {
            rel.tier = 0;
            result.push_back(rel);
            continue;
        }

        const float dx       = observerX - target.x;
        const float dy       = observerY - target.y;
        const float dist     = std::sqrt(dx * dx + dy * dy);
        const float safeDist = dist < 1.0f ? 1.0f : dist;
        const float combat   = inCombat[i] ? 1.0f : 0.0f;
        const float interest = (kBaseWeight + kCombatBonus * combat) / safeDist;

        if (interest >= kTier0Min)
            rel.tier = 0;
        else if (interest >= kTier1Min)
            rel.tier = 1;
        else
            rel.tier = 2;

        result.push_back(rel);
    }

    return result;
}

std::vector<EntityRelevance> PriorityEvaluator::Evaluate(
    uint32_t                             observerID,
    float                                observerX,
    float                                observerY,
    const std::vector<EvaluationTarget>& allEntities) const
{
    return Evaluate(observerID, observerX, observerY, allEntities,
                    ComputeInCombat(allEntities));
}

bool PriorityEvaluator::ShouldSend(uint8_t tier, uint32_t tickID) {
    if (tier == 0) return true;
    if (tier == 1) return (tickID % 2) == 0;
    return (tickID % 5) == 0;
}

}  // namespace NetworkMiddleware::Brain
