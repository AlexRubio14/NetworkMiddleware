#pragma once
// Core/HitValidator.h — P-5.3 Lag Compensation geometry helper.
//
// Header-only: pure arithmetic, no dependencies, no state.
// Used by main.cpp to validate ability hits against rewound target positions.

namespace NetworkMiddleware::Core {

    // Returns true if the distance between attacker and target is within range.
    // Uses squared-distance comparison to avoid a sqrt call.
    inline bool CheckHit(float atkX, float atkY,
                          float tgtX, float tgtY,
                          float range) {
        const float dx = atkX - tgtX;
        const float dy = atkY - tgtY;
        return (dx * dx + dy * dy) <= (range * range);
    }

}  // namespace NetworkMiddleware::Core
