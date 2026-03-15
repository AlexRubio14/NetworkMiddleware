#pragma once
#include <string>

namespace NetworkMiddleware::Brain {
    class IBehaviorEngine {
    public:
        virtual ~IBehaviorEngine() = default;
        virtual std::string Decide(float analysisResult) = 0;
    };
}