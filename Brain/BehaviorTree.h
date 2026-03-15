#pragma once
#include "Interfaces/IBehaviorEngine.h"
#include <string>

namespace NetworkMiddleware::Brain {
    class BehaviorTree : public IBehaviorEngine {
    public:
        std::string Decide(float analysisResult) override;
    };
}