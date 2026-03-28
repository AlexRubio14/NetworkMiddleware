#pragma once
#include "../Shared/ITransport.h"
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace NetworkMiddleware::Core {

    // P-6.2: Decouples the sendmmsg syscall from the game loop.
    //
    // The game loop (100Hz) calls Signal() after Phase B. A dedicated jthread
    // wakes on that signal (or after kFlushInterval at most) and calls
    // m_transport->Flush(), which dispatches all queued packets via sendmmsg.
    //
    // CommitAndSendBatchSnapshot still runs on the main thread — only the
    // actual syscall is moved off the critical path.
    class AsyncSendDispatcher {
    public:
        explicit AsyncSendDispatcher(std::shared_ptr<Shared::ITransport> transport);
        ~AsyncSendDispatcher();

        // Non-blocking. Notifies the send thread that packets are ready.
        // Called by the main thread after Phase B.
        void Signal();

    private:
        void ThreadLoop(std::stop_token st);

        std::shared_ptr<Shared::ITransport> m_transport;
        std::jthread                        m_thread;
        std::mutex                          m_cvMutex;
        std::condition_variable             m_cv;
        bool                                m_signaled{false};

        // Maximum latency before the send thread flushes without a Signal().
        // Matches the 30Hz snapshot rate (1000ms / 30 = ~33ms).
        static constexpr auto kFlushInterval = std::chrono::milliseconds(33);
    };

}  // namespace NetworkMiddleware::Core
