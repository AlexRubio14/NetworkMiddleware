// Brain/Brain.h
#pragma once
#include <memory>
#include <vector>
#include <string>
#include "Interfaces/IDataProcessor.h"
#include "Interfaces/IBehaviorEngine.h"

namespace NetworkMiddleware::Brain {

    /**
     * BrainManager — Phase 6+ orchestrator stub.
     * NOT connected to the main game loop.
     *
     * Active Brain components (used in the loop today):
     *   - KalmanPredictor  (P-5.2) — trajectory prediction on packet loss
     *   - PriorityEvaluator (P-5.4) — Tier 0/1/2 replication scheduling
     *
     * BrainManager will orchestrate NeuralProcessor + BehaviorTree in Fase 6
     * (Unreal Visual Bridge), once a real data pipeline exists.
     */
    class BrainManager {
    private:

        std::unique_ptr<IDataProcessor> m_processor;
        std::unique_ptr<IBehaviorEngine> m_behavior;

    public:
        BrainManager(std::unique_ptr<IDataProcessor> p, std::unique_ptr<IBehaviorEngine> b);
        std::string DecideAction(const std::vector<uint8_t>& data) const;
    };

}