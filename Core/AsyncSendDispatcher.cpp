#include "AsyncSendDispatcher.h"

namespace NetworkMiddleware::Core {

    AsyncSendDispatcher::AsyncSendDispatcher(std::shared_ptr<Shared::ITransport> transport)
        : m_transport(std::move(transport))
        , m_thread([this](std::stop_token st) { ThreadLoop(st); })
    {}

    AsyncSendDispatcher::~AsyncSendDispatcher() {
        // Request stop, wake the thread, then jthread destructor joins.
        m_thread.request_stop();
        m_cv.notify_one();
    }

    void AsyncSendDispatcher::Signal() {
        {
            std::lock_guard lock(m_cvMutex);
            m_signaled = true;
        }
        m_cv.notify_one();
    }

    void AsyncSendDispatcher::ThreadLoop(std::stop_token st) {
        while (!st.stop_requested()) {
            {
                std::unique_lock lock(m_cvMutex);
                // Wake on Signal() OR timeout (safety net at 30Hz) OR stop request.
                m_cv.wait_for(lock, kFlushInterval, [this, &st] {
                    return m_signaled || st.stop_requested();
                });
                m_signaled = false;
            }
            // Lock released before Flush() — sendmmsg runs without holding m_cvMutex.
            m_transport->Flush();
        }
        // Final drain: flush any packets queued between last Signal() and shutdown.
        m_transport->Flush();
    }

}  // namespace NetworkMiddleware::Core
