#pragma once
#include <cstdint>
#include <vector>

namespace NetworkMiddleware::Brain {
    class IDataProcessor {
    public:
        virtual ~IDataProcessor() = default;
        virtual float Analyze(const std::vector<uint8_t>& data) = 0;
    };
}
