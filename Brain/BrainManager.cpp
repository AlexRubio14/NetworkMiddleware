#include "BrainManager.h"

namespace NetworkMiddleware::Brain {

    BrainManager::BrainManager(std::unique_ptr<IDataProcessor> processor, std::unique_ptr<IBehaviorEngine> behavior)
        : m_processor(std::move(processor)), m_behavior(std::move(behavior)) {
    }

    std::string BrainManager::DecideAction(const std::vector<uint8_t>& data) const {
        float analysis = m_processor->Analyze(data);
        return m_behavior->Decide(analysis);
    }

}