#pragma once
#include "Interfaces/IDataProcessor.h"

namespace NetworkMiddleware::Brain {
    class NeuralProcessor : public IDataProcessor {
    public:
        float Analyze(const std::vector<uint8_t>& data) override;
    };
}