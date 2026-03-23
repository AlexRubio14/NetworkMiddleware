#pragma once
// Core/JobSystem.h — P-4.4 Dynamic Work-Stealing Job System
//
// Architecture:
//   WorkStealingQueue  — per-thread std::deque<task> + mutex.
//                        Owner: Push/Pop from the front (LIFO, cache-friendly).
//                        Thief: Steal from the back (FIFO, avoids head collision).
//
//   JobSystem          — manages N WorkerSlots (pre-allocated up to kMaxThreads).
//                        Dispatch is round-robin across active threads.
//                        Idle workers sleep on a shared condition_variable.
//                        Dynamic scaling via MaybeScale(recentAvgTickMs):
//                          > kUpscaleThresholdMs  → AddThread  (up to kMaxThreads)
//                          < kDownscaleThresholdMs → RemoveThread (down to kMinThreads)
//                          Downscale gated by kHysteresisDuration cooldown.
//
// Threading contract for Split-Phase snapshots:
//   Phase A (workers): SerializeSnapshotFor() — read-only on RemoteClient.
//   Phase B (main):    CommitAndSendSnapshot() — after std::latch::wait().
//   The main thread does NOT touch m_establishedClients during Phase A.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace NetworkMiddleware::Core {

// ─── WorkStealingQueue ────────────────────────────────────────────────────────
//
// Not copyable or movable (contains a mutex).
// One instance per WorkerSlot; pre-allocated in JobSystem constructor.
class WorkStealingQueue {
public:
    WorkStealingQueue()                                    = default;
    WorkStealingQueue(const WorkStealingQueue&)            = delete;
    WorkStealingQueue& operator=(const WorkStealingQueue&) = delete;

    // Push a task. Called by any thread that submits work (Execute / dispatch).
    void Push(std::function<void()> task);

    // Pop from the front — called by the owning worker (LIFO order).
    // Returns false if the queue is empty.
    bool Pop(std::function<void()>& out);

    // Steal from the back — called by a thief worker (FIFO order).
    // Returns false if the queue is empty.
    bool Steal(std::function<void()>& out);

    bool   Empty() const;
    size_t Size()  const;

private:
    mutable std::mutex                    m_mutex;
    std::deque<std::function<void()>>     m_tasks;
};

// ─── JobSystem ────────────────────────────────────────────────────────────────

class JobSystem {
public:
    // ── Scaling constants ────────────────────────────────────────────────────
    static constexpr size_t   kMinThreads               = 2;
    static constexpr float    kUpscaleThresholdMs        = 7.0f;
    static constexpr float    kDownscaleThresholdMs      = 3.0f;
    static constexpr auto     kHysteresisDuration        = std::chrono::seconds(5);
    static constexpr uint32_t kScaleCheckIntervalTicks   = 100;  // ~1s at 100 Hz

    // Logging callback injected at construction time.
    // Follows the project's no-global-state rule: JobSystem emits scale events
    // through this callback rather than calling the Logger singleton directly.
    // Pass nullptr (default) to silence scale-event logging (e.g. in tests).
    using ScaleLogFn = std::function<void(const std::string& msg)>;

    // Constructs the pool with `initialThreads` active workers.
    // Pre-allocates slots up to hardware_concurrency-1 (min kMinThreads).
    // logFn: optional callback for scale-event messages (nullptr = silent).
    explicit JobSystem(size_t initialThreads = kMinThreads, ScaleLogFn logFn = nullptr);

    // Destructor: requests stop on all active threads and joins them.
    ~JobSystem();

    // Submit a task for asynchronous execution.
    // Dispatches round-robin to the active thread queues and wakes an idle worker.
    void Execute(std::function<void()> task);

    // Called once per game tick from the main thread.
    // Every kScaleCheckIntervalTicks ticks it evaluates recentAvgTickMs against
    // the scaling thresholds and adds or removes one thread accordingly.
    void MaybeScale(float recentAvgTickMs);

    size_t   GetThreadCount() const noexcept;
    uint64_t GetStealCount()  const noexcept;

    // ── Test hooks (bypass the tick-interval guard) ───────────────────────────
    void ForceAddThread();
    void ForceRemoveThread();

private:
    // ── Per-thread state ─────────────────────────────────────────────────────
    struct WorkerSlot {
        WorkStealingQueue           queue;
        std::atomic<bool>           shouldRun{false};
        std::optional<std::jthread> thread;

        WorkerSlot()                             = default;
        WorkerSlot(const WorkerSlot&)            = delete;
        WorkerSlot& operator=(const WorkerSlot&) = delete;
    };

    // ── Members ───────────────────────────────────────────────────────────────
    ScaleLogFn                                   m_logFn;
    const size_t                                 m_maxThreads;
    std::vector<std::unique_ptr<WorkerSlot>>     m_slots;       // pre-allocated, never reallocated
    std::atomic<size_t>                          m_activeCount{0};
    std::atomic<size_t>                          m_dispatchIdx{0};  // round-robin cursor

    std::mutex                                   m_scaleMutex;
    std::chrono::steady_clock::time_point        m_lastDownscaleTime{};
    uint32_t                                     m_scaleTick{0};

    std::atomic<uint64_t>                        m_stealCount{0};

    // Shared wake signal — notified when a task is pushed or a thread must exit.
    std::condition_variable                      m_cv;
    std::mutex                                   m_cvMutex;

    // ── Internal ──────────────────────────────────────────────────────────────
    void WorkerLoop(size_t myIdx, std::stop_token stop);
    void AddThread();      // must be called with m_scaleMutex held
    void RemoveThread();   // must be called with m_scaleMutex held
};

} // namespace NetworkMiddleware::Core
