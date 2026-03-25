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
    // P-4.4: m_recentAvgTickUs uses EMA(α=0.1) updated by RecordTick() on the main
    // thread only. Written single-threaded, read by JobSystem::MaybeScale() also on
    // the main thread → no CAS needed, relaxed ordering sufficient.
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
            uint32_t crcErrors          = 0;  // P-4.5: packets discarded due to CRC mismatch
            float    avgTickTimeUs      = 0.0f;
            float    avgFullTickTimeUs  = 0.0f;  // full game loop (Update + GameWorld + snapshots)
            float    deltaEfficiency    = 0.0f;  // 1 - (actual_avg_bytes / theoretical_full_sync_bytes)
            float    recentAvgTickMs    = 0.0f;  // P-4.4: EMA(α=0.1) — reactive to load spikes
        };

        // Full sync per entity-snapshot wire packet:
        //   104 bits PacketHeader = 13 bytes
        //   149 bits payload      = 19 bytes (ceil)
        //     4 bytes CRC32 trailer
        //   ─────────────────────────── = 36 bytes total
        // Using only payload+CRC (23) caused actual > theoretical when header overhead
        // was included in totalBytesSent, clamping Delta Efficiency to 0%.
        static constexpr uint32_t kFullSyncBytesPerClient = 36;

        // EMA smoothing factor — α=0.1 gives a half-life of ~7 ticks at 100 Hz.
        static constexpr float kEmaAlpha = 0.1f;

        // Declarations only — implementations in NetworkProfiler.cpp.
        void RecordBytesSent(size_t bytes) noexcept;
        void RecordBytesReceived(size_t bytes) noexcept;
        void IncrementRetransmissions() noexcept;
        void IncrementCRCErrors() noexcept;

        // P-5.x: Records how many entity-snapshots were dispatched this tick.
        // Used to compute an accurate theoretical full-sync baseline for Delta
        // Efficiency — since P-5.1 sends N entities per client (not just 1),
        // the old connectedClients×kFullSyncBytesPerClient denominator was wrong.
        void RecordEntitySnapshotsSent(size_t count) noexcept;

        // P-5.x: Records bytes sent exclusively for entity-snapshot packets.
        // Delta Efficiency uses this (not totalBytesSent) so that control traffic
        // (heartbeats, handshake, ACKs) does not inflate the actual-bytes numerator
        // and incorrectly clamp efficiency to 0%.
        void RecordSnapshotBytesSent(size_t bytes) noexcept;

        // RecordTick: updates cumulative average AND the EMA reactive average.
        // Must be called from the main thread (single-threaded write to m_recentAvgTickUs).
        // Measures NetworkManager::Update() only (receive loop + handshakes + ACKs).
        void RecordTick(uint64_t microseconds) noexcept;

        // RecordFullTick: cumulative average of the COMPLETE game-loop iteration
        // (Update + GameWorld::Tick + Phase 0/0b/A/B snapshot pipeline).
        // Called from main.cpp after Phase B and before sleep_until.
        void RecordFullTick(uint64_t microseconds) noexcept;

        // Returns the EMA-smoothed recent tick time in microseconds.
        // Safe to call from any thread (relaxed atomic read).
        float GetRecentAvgTickUs() const noexcept;

        // Test hook: override the EMA value without going through RecordTick().
        // Allows scaling-threshold tests to inject synthetic load without timing.
        void SetRecentAvgTickUsForTest(float valueUs) noexcept;

        // Prints a summary report via Logger if kReportInterval (5s) has elapsed.
        // Thread-safe: m_reportMutex prevents duplicate reports from concurrent callers.
        void MaybeReport(size_t connectedClients);

        // Returns a snapshot of current cumulative counters.
        Snapshot GetSnapshot(size_t connectedClients) const;

    private:
        std::atomic<uint64_t> m_bytesSent{0};
        std::atomic<uint64_t> m_bytesReceived{0};
        std::atomic<uint32_t> m_retransmissions{0};
        std::atomic<uint32_t> m_crcErrors{0};
        std::atomic<uint64_t> m_tickTimeAccumUs{0};
        std::atomic<uint32_t> m_tickCount{0};
        std::atomic<uint64_t> m_fullTickTimeAccumUs{0};
        std::atomic<uint32_t> m_fullTickCount{0};
        std::atomic<uint64_t> m_entitySnapshotsSent{0};
        std::atomic<uint64_t> m_snapshotBytesSent{0};  // snapshot-only bytes for DeltaEfficiency

        // P-4.4: EMA reactive tick time. Written only by RecordTick() (main thread).
        // std::atomic<float> is standard since C++20; store/load with relaxed ordering.
        std::atomic<float>    m_recentAvgTickUs{0.0f};

        // Protected by m_reportMutex: not atomic, accessed in check-then-act.
        mutable std::mutex                    m_reportMutex;
        std::chrono::steady_clock::time_point m_startTime{
            std::chrono::steady_clock::now()};
        std::chrono::steady_clock::time_point m_lastReportTime{
            std::chrono::steady_clock::now()};

        static constexpr auto kReportInterval = std::chrono::seconds(5);
    };

} // namespace NetworkMiddleware::Core
