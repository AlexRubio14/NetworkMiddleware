#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace NetworkMiddleware::Core {

    // Thread-safe network telemetry collector for P-4.3 Stress Test.
    //
    // All counters use std::atomic<> to be ready for the Phase 4.4 thread pool
    // (callbacks and packet dispatch will run on worker threads).
    //
    // Usage (from NetworkManager::Update()):
    //   m_profiler.RecordBytesReceived(buffer.size());   // after each Receive()
    //   m_profiler.RecordBytesSent(compressed.size());   // inside Send()
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

        // Full sync: 145 bits ≈ 19 bytes per client (P-2 validated result).
        static constexpr uint32_t kFullSyncBytesPerClient = 19;

        void RecordBytesSent(size_t bytes) noexcept {
            m_bytesSent.fetch_add(bytes, std::memory_order_relaxed);
        }

        void RecordBytesReceived(size_t bytes) noexcept {
            m_bytesReceived.fetch_add(bytes, std::memory_order_relaxed);
        }

        void IncrementRetransmissions() noexcept {
            m_retransmissions.fetch_add(1, std::memory_order_relaxed);
        }

        void RecordTick(uint64_t microseconds) noexcept {
            m_tickTimeAccumUs.fetch_add(microseconds, std::memory_order_relaxed);
            m_tickCount.fetch_add(1, std::memory_order_relaxed);
        }

        // Prints a summary report via Logger if kReportInterval (5s) has elapsed.
        // connectedClients is needed to compute the theoretical full-sync bandwidth.
        void MaybeReport(size_t connectedClients);

        // Returns a snapshot of current cumulative counters.
        Snapshot GetSnapshot(size_t connectedClients) const;

    private:
        std::atomic<uint64_t> m_bytesSent{0};
        std::atomic<uint64_t> m_bytesReceived{0};
        std::atomic<uint32_t> m_retransmissions{0};
        std::atomic<uint64_t> m_tickTimeAccumUs{0};
        std::atomic<uint32_t> m_tickCount{0};

        std::chrono::steady_clock::time_point m_startTime{
            std::chrono::steady_clock::now()};
        std::chrono::steady_clock::time_point m_lastReportTime{
            std::chrono::steady_clock::now()};

        static constexpr auto kReportInterval = std::chrono::seconds(5);
    };

} // namespace NetworkMiddleware::Core
