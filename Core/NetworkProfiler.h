#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace NetworkMiddleware::Core {

    // Thread-safe network telemetry collector for P-4.3 Stress Test.
    //
    // Atomic counters are safe for concurrent access (ready for P-4.4 thread pool).
    // m_startTime and m_lastReportTime are protected by m_reportMutex because
    // std::chrono::time_point is not atomic and MaybeReport() does a check-then-act.
    //
    // Usage (from NetworkManager::Update()):
    //   m_profiler.RecordBytesReceived(buffer.size());   // after each Receive()
    //   m_profiler.RecordBytesSent(compressed.size());   // after each Send/resend
    //   m_profiler.IncrementRetransmissions();           // inside ResendPendingPackets()
    //   m_profiler.RecordTick(tickMicros);               // end of Update()
    //   m_profiler.MaybeReport(GetEstablishedCount());   // end of Update()
    class NetworkProfiler {
    public:
        // Point-in-time snapshot (reads are individually atomic, not collectively atomic).
        struct Snapshot {
            uint64_t totalBytesSent     = 0;
            uint64_t totalBytesReceived = 0;
            uint32_t retransmissions    = 0;
            float    avgTickTimeUs      = 0.0f;
            float    deltaEfficiency    = 0.0f;  // 1 - (actual_avg_bytes / theoretical_full_sync_bytes)
        };

        // Full sync: 149 bits ≈ 19 bytes per client (P-2/P-3.5 validated result).
        static constexpr uint32_t kFullSyncBytesPerClient = 19;

        // Declarations only — implementations in NetworkProfiler.cpp.
        void RecordBytesSent(size_t bytes) noexcept;
        void RecordBytesReceived(size_t bytes) noexcept;
        void IncrementRetransmissions() noexcept;
        void RecordTick(uint64_t microseconds) noexcept;

        // Prints a summary report via Logger if kReportInterval (5s) has elapsed.
        // Thread-safe: m_reportMutex prevents duplicate reports from concurrent callers.
        void MaybeReport(size_t connectedClients);

        // Returns a snapshot of current cumulative counters.
        Snapshot GetSnapshot(size_t connectedClients) const;

    private:
        std::atomic<uint64_t> m_bytesSent{0};
        std::atomic<uint64_t> m_bytesReceived{0};
        std::atomic<uint32_t> m_retransmissions{0};
        std::atomic<uint64_t> m_tickTimeAccumUs{0};
        std::atomic<uint32_t> m_tickCount{0};

        // Protected by m_reportMutex: not atomic, accessed in check-then-act.
        mutable std::mutex                    m_reportMutex;
        std::chrono::steady_clock::time_point m_startTime{
            std::chrono::steady_clock::now()};
        std::chrono::steady_clock::time_point m_lastReportTime{
            std::chrono::steady_clock::now()};

        static constexpr auto kReportInterval = std::chrono::seconds(5);
    };

} // namespace NetworkMiddleware::Core
