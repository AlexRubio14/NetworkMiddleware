// Brain/Brain.h
#pragma once
#include <memory>
#include <vector>
#include <string>
#include "Interfaces/IDataProcessor.h"
#include "Interfaces/IBehaviorEngine.h"

namespace NetworkMiddleware::Brain {

    class BrainManager {
    private:

        std::unique_ptr<IDataProcessor> m_processor;
        std::unique_ptr<IBehaviorEngine> m_behavior;

    public:
        BrainManager(std::unique_ptr<IDataProcessor> p, std::unique_ptr<IBehaviorEngine> b);
        std::string DecideAction(const std::vector<uint8_t>& data) const;
    };

}