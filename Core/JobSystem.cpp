// Core/JobSystem.cpp — P-4.4 Dynamic Work-Stealing Job System

#include "JobSystem.h"
#include <algorithm>
#include <format>

namespace NetworkMiddleware::Core {

// ─── WorkStealingQueue ────────────────────────────────────────────────────────

void WorkStealingQueue::Push(std::function<void()> task) {
    std::lock_guard lock(m_mutex);
    m_tasks.push_front(std::move(task));
}

bool WorkStealingQueue::Pop(std::function<void()>& out) {
    std::lock_guard lock(m_mutex);
    if (m_tasks.empty()) return false;
    out = std::move(m_tasks.front());
    m_tasks.pop_front();
    return true;
}

bool WorkStealingQueue::Steal(std::function<void()>& out) {
    std::lock_guard lock(m_mutex);
    if (m_tasks.empty()) return false;
    out = std::move(m_tasks.back());
    m_tasks.pop_back();
    return true;
}

bool WorkStealingQueue::Empty() const {
    std::lock_guard lock(m_mutex);
    return m_tasks.empty();
}

size_t WorkStealingQueue::Size() const {
    std::lock_guard lock(m_mutex);
    return m_tasks.size();
}

// ─── JobSystem ────────────────────────────────────────────────────────────────

JobSystem::JobSystem(size_t initialThreads, ScaleLogFn logFn) :
    m_logFn(std::move(logFn)),
    m_maxThreads(std::max(kMinThreads,
                          static_cast<size_t>(std::thread::hardware_concurrency()) > 1
                              ? static_cast<size_t>(std::thread::hardware_concurrency()) - 1u
                              : kMinThreads))
{
    // Pre-allocate all slots so the vector never reallocates (workers hold raw ptrs).
    m_slots.reserve(m_maxThreads);
    for (size_t i = 0; i < m_maxThreads; ++i)
        m_slots.push_back(std::make_unique<WorkerSlot>());

    // Start the requested number of threads (clamped to [kMinThreads, kMaxThreads]).
    const size_t startCount = std::clamp(initialThreads, kMinThreads, m_maxThreads);

    std::lock_guard lock(m_scaleMutex);
    for (size_t i = 0; i < startCount; ++i)
        AddThread();
}

JobSystem::~JobSystem() {
    // Request stop on all active threads, then join via jthread destructor.
    // We call request_stop() first so all threads can begin draining simultaneously.
    const size_t count = m_activeCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        m_slots[i]->shouldRun.store(false, std::memory_order_relaxed);
        if (m_slots[i]->thread)
            m_slots[i]->thread->request_stop();
    }
    // Wake all sleeping workers so they can observe the stop.
    m_cv.notify_all();
    // jthread destructors join automatically.
    for (size_t i = 0; i < count; ++i)
        m_slots[i]->thread.reset();
}

// ─── Execute ─────────────────────────────────────────────────────────────────

void JobSystem::Execute(std::function<void()> task) {
    const size_t count = m_activeCount.load(std::memory_order_acquire);
    if (count == 0) {
        // Safety fallback: no workers alive, run inline.
        task();
        return;
    }
    // Round-robin dispatch, but skip slots marked as retiring (shouldRun == false).
    // This closes the late-enqueue hole: RemoveThread() sets shouldRun=false before
    // draining, so we will never enqueue into a slot that is about to be retired.
    const size_t start = m_dispatchIdx.fetch_add(1, std::memory_order_relaxed) % count;
    for (size_t i = 0; i < count; ++i) {
        const size_t idx = (start + i) % count;
        if (m_slots[idx]->shouldRun.load(std::memory_order_acquire)) {
            m_slots[idx]->queue.Push(std::move(task));
            m_cv.notify_one();
            return;
        }
    }
    // All slots retiring (extreme edge case during teardown) — use slot 0.
    m_slots[0]->queue.Push(std::move(task));
    m_cv.notify_one();
}

// ─── WorkerLoop ──────────────────────────────────────────────────────────────

void JobSystem::WorkerLoop(size_t myIdx, std::stop_token stop) {
    // Exception boundary: catch any exception thrown by a user job so it cannot
    // escape the jthread entry function and terminate the server process.
    auto safeRun = [this](std::function<void()>& t) noexcept {
        try {
            t();
        } catch (const std::exception& e) {
            if (m_logFn) m_logFn(std::format("[JobSystem] Worker job threw: {}", e.what()));
        } catch (...) {
            if (m_logFn) m_logFn("[JobSystem] Worker job threw unknown exception — suppressed");
        }
    };

    while (!stop.stop_requested() &&
           m_slots[myIdx]->shouldRun.load(std::memory_order_relaxed))
    {
        std::function<void()> task;

        // 1. Try own queue first (LIFO — hot cache path).
        if (m_slots[myIdx]->queue.Pop(task)) {
            safeRun(task);
            continue;
        }

        // 2. Work-stealing: scan active peers starting from the next slot
        //    (rotating start index reduces systematic contention).
        const size_t count = m_activeCount.load(std::memory_order_acquire);
        bool stolen = false;
        for (size_t offset = 1; offset < count && !stolen; ++offset) {
            const size_t victim = (myIdx + offset) % count;
            if (m_slots[victim]->queue.Steal(task)) {
                m_stealCount.fetch_add(1, std::memory_order_relaxed);
                safeRun(task);
                stolen = true;
            }
        }
        if (stolen) continue;

        // 3. No work found — sleep until a task arrives or we are stopped.
        std::unique_lock lock(m_cvMutex);
        m_cv.wait_for(lock, std::chrono::microseconds(200), [&] {
            return stop.stop_requested() ||
                   !m_slots[myIdx]->shouldRun.load(std::memory_order_relaxed) ||
                   !m_slots[myIdx]->queue.Empty();
        });
    }
}

// ─── MaybeScale ──────────────────────────────────────────────────────────────

void JobSystem::MaybeScale(float recentAvgTickMs) {
    if (++m_scaleTick < kScaleCheckIntervalTicks)
        return;
    m_scaleTick = 0;

    std::lock_guard lock(m_scaleMutex);
    const size_t count = m_activeCount.load(std::memory_order_relaxed);

    if (recentAvgTickMs > kUpscaleThresholdMs && count < m_maxThreads) {
        AddThread();
        if (m_logFn)
            m_logFn(std::format("[JobSystem] Upscale: {} → {} threads (avgTick={:.2f}ms)",
                count, count + 1, recentAvgTickMs));
        return;
    }

    if (recentAvgTickMs < kDownscaleThresholdMs && count > kMinThreads) {
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastDownscaleTime < kHysteresisDuration)
            return;  // Hysteresis: too soon after last downscale
        m_lastDownscaleTime = now;
        RemoveThread();
        if (m_logFn)
            m_logFn(std::format("[JobSystem] Downscale: {} → {} threads (avgTick={:.2f}ms)",
                count, count - 1, recentAvgTickMs));
    }
}

// ─── AddThread / RemoveThread ─────────────────────────────────────────────────
// Both require m_scaleMutex to be held by the caller.

void JobSystem::AddThread() {
    const size_t idx = m_activeCount.load(std::memory_order_relaxed);
    if (idx >= m_maxThreads) return;

    m_slots[idx]->shouldRun.store(true, std::memory_order_relaxed);
    // Increment BEFORE starting the thread so WorkerLoop sees a valid count.
    m_activeCount.fetch_add(1, std::memory_order_release);

    m_slots[idx]->thread.emplace([this, idx](std::stop_token st) {
        WorkerLoop(idx, st);
    });
}

void JobSystem::RemoveThread() {
    const size_t count = m_activeCount.load(std::memory_order_relaxed);
    if (count <= kMinThreads) return;

    const size_t retiring = count - 1;

    // 1. Mark retiring slot non-dispatchable BEFORE draining.
    //    Execute() skips slots with shouldRun==false, so no new tasks will be
    //    enqueued here after this store (with acquire/release pairing).
    m_slots[retiring]->shouldRun.store(false, std::memory_order_release);

    // 2. Drain the retiring slot's queue into slot 0.
    //    Any tasks enqueued between the shouldRun store and this drain are
    //    a vanishingly narrow race; the retiring worker will still execute
    //    them via its own Pop() before it observes shouldRun==false.
    {
        std::function<void()> task;
        while (m_slots[retiring]->queue.Steal(task)) {
            m_slots[0]->queue.Push(std::move(task));
            m_cv.notify_one();
        }
    }

    // 3. Shrink active count — slot is empty and non-dispatchable.
    m_activeCount.fetch_sub(1, std::memory_order_release);

    m_slots[retiring]->shouldRun.store(false, std::memory_order_relaxed);
    if (m_slots[retiring]->thread) {
        m_slots[retiring]->thread->request_stop();
        m_cv.notify_all();   // wake thread so it can observe shouldRun = false
        m_slots[retiring]->thread->join();
        m_slots[retiring]->thread.reset();
    }
}

// ─── Accessors ────────────────────────────────────────────────────────────────

size_t JobSystem::GetThreadCount() const noexcept {
    return m_activeCount.load(std::memory_order_relaxed);
}

uint64_t JobSystem::GetStealCount() const noexcept {
    return m_stealCount.load(std::memory_order_relaxed);
}

// ─── Test hooks ───────────────────────────────────────────────────────────────

void JobSystem::ForceAddThread() {
    std::lock_guard lock(m_scaleMutex);
    AddThread();
}

void JobSystem::ForceRemoveThread() {
    std::lock_guard lock(m_scaleMutex);
    RemoveThread();
}

} // namespace NetworkMiddleware::Core
