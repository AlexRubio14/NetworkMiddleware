#include "BehaviorTree.h"

namespace NetworkMiddleware::Brain {
    std::string BehaviorTree::Decide(float analysisResult) {
        if (analysisResult > 0.5f)
            return "EXECUTE_BRAIN_LOGIC";

        return "IDLE";
    }
}