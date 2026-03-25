#include "HitValidator.h"

namespace NetworkMiddleware::Core {

bool CheckHit(float atkX, float atkY,
              float tgtX, float tgtY,
              float range) {
    const float dx = atkX - tgtX;
    const float dy = atkY - tgtY;
    return (dx * dx + dy * dy) <= (range * range);
}

}  // namespace NetworkMiddleware::Core
