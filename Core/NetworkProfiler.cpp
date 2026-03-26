#include "NetworkProfiler.h"
#include "../Shared/Log/Logger.h"
#include <algorithm>
#include <format>

namespace NetworkMiddleware::Core {

    // ── Record methods ────────────────────────────────────────────────────────
    // Moved from header per coding guidelines (no implementation in headers).
    // Trivial single-line atomics; compiler will inline them from this TU anyway.

    void NetworkProfiler::RecordBytesSent(size_t bytes) noexcept {
        m_bytesSent.fetch_add(bytes, std::memory_order_relaxed);
    }

    void NetworkProfiler::RecordBytesReceived(size_t bytes) noexcept {
        m_bytesReceived.fetch_add(bytes, std::memory_order_relaxed);
    }

    void NetworkProfiler::IncrementRetransmissions() noexcept {
        m_retransmissions.fetch_add(1, std::memory_order_relaxed);
    }

    void NetworkProfiler::IncrementCRCErrors() noexcept {
        m_crcErrors.fetch_add(1, std::memory_order_relaxed);
    }

    void NetworkProfiler::RecordEntitySnapshotsSent(size_t count) noexcept {
        m_entitySnapshotsSent.fetch_add(count, std::memory_order_relaxed);
    }

    void NetworkProfiler::RecordSnapshotBytesSent(size_t bytes) noexcept {
        m_snapshotBytesSent.fetch_add(bytes, std::memory_order_relaxed);
    }

    void NetworkProfiler::RecordTick(uint64_t microseconds) noexcept {
        m_tickTimeAccumUs.fetch_add(microseconds, std::memory_order_relaxed);
        m_tickCount.fetch_add(1, std::memory_order_relaxed);

        // P-4.4: EMA(α=0.1) — single-threaded write, relaxed ordering sufficient.
        const float current = static_cast<float>(microseconds);
        const float old     = m_recentAvgTickUs.load(std::memory_order_relaxed);
        m_recentAvgTickUs.store(kEmaAlpha * current + (1.0f - kEmaAlpha) * old,
                                std::memory_order_relaxed);
    }

    void NetworkProfiler::RecordFullTick(uint64_t microseconds) noexcept {
        m_fullTickTimeAccumUs.fetch_add(microseconds, std::memory_order_relaxed);
        m_fullTickCount.fetch_add(1, std::memory_order_relaxed);
    }

    float NetworkProfiler::GetRecentAvgTickUs() const noexcept {
        return m_recentAvgTickUs.load(std::memory_order_relaxed);
    }

    void NetworkProfiler::SetRecentAvgTickUsForTest(float valueUs) noexcept {
        m_recentAvgTickUs.store(valueUs, std::memory_order_relaxed);
    }

    // ── GetSnapshot ───────────────────────────────────────────────────────────

    NetworkProfiler::Snapshot NetworkProfiler::GetSnapshot(size_t connectedClients) const {
        Snapshot s;
        s.totalBytesSent     = m_bytesSent.load(std::memory_order_relaxed);
        s.totalBytesReceived = m_bytesReceived.load(std::memory_order_relaxed);
        s.retransmissions    = m_retransmissions.load(std::memory_order_relaxed);
        s.crcErrors          = m_crcErrors.load(std::memory_order_relaxed);

        const uint32_t ticks = m_tickCount.load(std::memory_order_relaxed);
        const uint64_t accum = m_tickTimeAccumUs.load(std::memory_order_relaxed);
        s.avgTickTimeUs  = (ticks > 0)
            ? static_cast<float>(accum) / static_cast<float>(ticks)
            : 0.0f;
        s.recentAvgTickMs = m_recentAvgTickUs.load(std::memory_order_relaxed) / 1000.0f;

        const uint32_t fullTicks = m_fullTickCount.load(std::memory_order_relaxed);
        const uint64_t fullAccum = m_fullTickTimeAccumUs.load(std::memory_order_relaxed);
        s.avgFullTickTimeUs = (fullTicks > 0)
            ? static_cast<float>(fullAccum) / static_cast<float>(fullTicks)
            : 0.0f;

        // Delta Efficiency = 1 - (snapshot_bytes_per_tick / theoretical_full_sync_per_tick)
        //
        // P-5.x: uses m_snapshotBytesSent (snapshot packets only) instead of
        // m_bytesSent (all traffic) so control overhead (heartbeats, handshake,
        // ACKs) does not inflate the actual numerator and clamp efficiency to 0%.
        // theoretical = (entitySnapshotsSent / ticks) × kFullSyncBytesPerClient.
        const uint64_t entitySnapshots  = m_entitySnapshotsSent.load(std::memory_order_relaxed);
        const uint64_t snapshotBytes    = m_snapshotBytesSent.load(std::memory_order_relaxed);
        if (ticks > 0 && entitySnapshots > 0) {
            const float actualAvg       = static_cast<float>(snapshotBytes)
                                          / static_cast<float>(ticks);
            const float entitiesPerTick = static_cast<float>(entitySnapshots)
                                          / static_cast<float>(ticks);
            const float theoretical     = static_cast<float>(kFullSyncBytesPerClient)
                                          * entitiesPerTick;
            s.deltaEfficiency = std::max(0.0f, 1.0f - (actualAvg / theoretical));
        }

        return s;
    }

    // ── MaybeReport ───────────────────────────────────────────────────────────

    void NetworkProfiler::MaybeReport(size_t connectedClients) {
        const auto now = std::chrono::steady_clock::now();

        // Lock prevents two concurrent callers from both passing the interval check
        // and emitting duplicate reports (relevant once P-4.4 thread pool is active).
        std::lock_guard<std::mutex> lock(m_reportMutex);
        if (now - m_lastReportTime < kReportInterval)
            return;

        m_lastReportTime = now;

        const Snapshot s = GetSnapshot(connectedClients);

        // Compute average in/out rates since profiler start (cumulative / elapsed).
        const float elapsedSec = std::chrono::duration<float>(
            now - m_startTime).count();
        const float sentKbps = (elapsedSec > 0.0f)
            ? (static_cast<float>(s.totalBytesSent)     * 8.0f / 1000.0f / elapsedSec)
            : 0.0f;
        const float recvKbps = (elapsedSec > 0.0f)
            ? (static_cast<float>(s.totalBytesReceived) * 8.0f / 1000.0f / elapsedSec)
            : 0.0f;

        Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
            std::format(
                "[PROFILER] Clients: {} | Avg Tick: {:.2f}ms | Full Loop: {:.2f}ms | Out: {:.1f}kbps | In: {:.1f}kbps | Retries: {} | CRC Err: {} | Delta Efficiency: {:.0f}%",
                connectedClients,
                s.avgTickTimeUs / 1000.0f,     // µs → ms  (NetworkManager::Update only)
                s.avgFullTickTimeUs / 1000.0f, // µs → ms  (complete game loop iteration)
                sentKbps,
                recvKbps,
                s.retransmissions,
                s.crcErrors,
                s.deltaEfficiency * 100.0f));
    }

} // namespace NetworkMiddleware::Core
