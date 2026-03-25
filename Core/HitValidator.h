#pragma once
// Core/HitValidator.h — P-5.3 Lag Compensation geometry helper.
// Implementation lives in HitValidator.cpp (project rule: no bodies in headers).

namespace NetworkMiddleware::Core {

    // Returns true if the squared distance between attacker and target is within range².
    // Avoids a sqrt call by comparing squared distances.
    bool CheckHit(float atkX, float atkY,
                  float tgtX, float tgtY,
                  float range);

}  // namespace NetworkMiddleware::Core
