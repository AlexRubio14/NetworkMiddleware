#pragma once
#include "LogLevel.h"
#include "LogChannel.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <condition_variable>
#include <thread>
#include <chrono>


namespace NetworkMiddleware::Shared {

    // How raw packet bytes are displayed when LogPacket() is called.
    enum class DumpMode {
        Hex,        // Wireshark-style hex + ASCII  (8 bytes/row)
        Binary,     // Per-byte: offset | hex | binary | decimal | char
        HexBinary,  // Combined: hex + binary side-by-side (4 bytes/row)
    };

    struct LogEntry {
        LogLevel   level;
        LogChannel channel;
        std::string message;
        std::shared_ptr<std::vector<uint8_t>> rawData;
        std::chrono::system_clock::time_point timestamp;
        DumpMode   dumpMode   = DumpMode::Hex;
        // Set only for SYNC sentinel entries — called by consumer when entry is processed.
        std::function<void()> syncCallback;
    };
}