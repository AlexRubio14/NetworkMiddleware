#pragma once
#include "LogLevel.h"
#include "LogChannel.h"
#include <string>
#include <vector>
#include <memory>
#include <condition_variable>
#include <thread>
#include <chrono>


namespace NetworkMiddleware::Shared {
    struct LogEntry {
        LogLevel level;
        LogChannel channel;
        std::string message;
        std::shared_ptr<std::vector<uint8_t>> rawData;
        std::chrono::system_clock::time_point timestamp;
    };
}