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

    void NetworkProfiler::RecordTick(uint64_t microseconds) noexcept {
        m_tickTimeAccumUs.fetch_add(microseconds, std::memory_order_relaxed);
        m_tickCount.fetch_add(1, std::memory_order_relaxed);
    }

    // ── GetSnapshot ───────────────────────────────────────────────────────────

    NetworkProfiler::Snapshot NetworkProfiler::GetSnapshot(size_t connectedClients) const {
        Snapshot s;
        s.totalBytesSent     = m_bytesSent.load(std::memory_order_relaxed);
        s.totalBytesReceived = m_bytesReceived.load(std::memory_order_relaxed);
        s.retransmissions    = m_retransmissions.load(std::memory_order_relaxed);

        const uint32_t ticks = m_tickCount.load(std::memory_order_relaxed);
        const uint64_t accum = m_tickTimeAccumUs.load(std::memory_order_relaxed);
        s.avgTickTimeUs = (ticks > 0)
            ? static_cast<float>(accum) / static_cast<float>(ticks)
            : 0.0f;

        // Delta Efficiency = 1 - (avg_bytes_sent_per_tick / theoretical_full_sync_bytes_per_tick)
        // theoretical = kFullSyncBytesPerClient * connectedClients per tick
        if (ticks > 0 && connectedClients > 0) {
            const float actualAvg   = static_cast<float>(s.totalBytesSent)
                                      / static_cast<float>(ticks);
            const float theoretical = static_cast<float>(kFullSyncBytesPerClient)
                                      * static_cast<float>(connectedClients);
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

        // Compute average output rate since profiler start (cumulative / elapsed).
        const float elapsedSec = std::chrono::duration<float>(
            now - m_startTime).count();
        const float sentKbps = (elapsedSec > 0.0f)
            ? (static_cast<float>(s.totalBytesSent) * 8.0f / 1000.0f / elapsedSec)
            : 0.0f;

        Shared::Logger::Log(Shared::LogLevel::Info, Shared::LogChannel::Core,
            std::format(
                "[PROFILER] Clients: {} | Avg Tick: {:.2f}ms | Out: {:.1f}kbps | Retries: {} | Delta Efficiency: {:.0f}%",
                connectedClients,
                s.avgTickTimeUs / 1000.0f,   // µs → ms
                sentKbps,
                s.retransmissions,
                s.deltaEfficiency * 100.0f));
    }

} // namespace NetworkMiddleware::Core
