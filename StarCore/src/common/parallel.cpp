#include <algorithm>

#include "starcore/common/assert.hpp"
#include "starcore/common/parallel.hpp"

namespace starcore
{

ParallelExecutor::ParallelExecutor(unsigned threadCount)
{
    if (threadCount == 0) {
        threadCount = std::max(1U, std::thread::hardware_concurrency());
    }
    m_workers.reserve(threadCount - 1);
    for (unsigned i = 1; i < threadCount; ++i) {
        m_workers.emplace_back(
            [this](const std::stop_token& token) { workerLoop(token); });
    }
}

ParallelExecutor::~ParallelExecutor()
{
    for (auto& worker : m_workers) {
        worker.request_stop();  // Wakes the stop_token-aware wait.
    }
    // Join now: the workers use m_mutex/m_wakeWorkers, which must outlive
    // them.
    m_workers.clear();
}

void ParallelExecutor::run(std::size_t count, InvokeFn invoke, void* context)
{
    if (count == 0) {
        return;
    }
    {
        std::scoped_lock lock(m_mutex);
        SC_ASSERT(m_activeWorkers == 0, "ParallelExecutor is not reentrant");
        m_invoke = invoke;
        m_context = context;
        m_count = count;
        m_nextIndex.store(0, std::memory_order_relaxed);
        m_exception = nullptr;
        m_activeWorkers = static_cast<unsigned>(m_workers.size());
        ++m_generation;
    }
    m_wakeWorkers.notify_all();

    processIndices();

    std::exception_ptr exception;
    {
        std::unique_lock lock(m_mutex);
        m_jobDone.wait(lock, [this] { return m_activeWorkers == 0; });
        exception = std::exchange(m_exception, nullptr);
    }
    if (exception) {
        std::rethrow_exception(exception);
    }
}

void ParallelExecutor::workerLoop(const std::stop_token& stopToken)
{
    std::size_t seenGeneration = 0;
    while (true) {
        {
            std::unique_lock lock(m_mutex);
            if (!m_wakeWorkers.wait(lock, stopToken, [&] {
                    return m_generation != seenGeneration;
                })) {
                return;  // Stop requested.
            }
            seenGeneration = m_generation;
        }

        processIndices();

        {
            std::scoped_lock lock(m_mutex);
            if (--m_activeWorkers == 0) {
                m_jobDone.notify_one();
            }
        }
    }
}

void ParallelExecutor::processIndices()
{
    while (true) {
        const std::size_t index =
            m_nextIndex.fetch_add(1, std::memory_order_relaxed);
        if (index >= m_count) {
            return;
        }
        try {
            m_invoke(m_context, index);
        } catch (...) {
            std::scoped_lock lock(m_mutex);
            if (!m_exception) {
                m_exception = std::current_exception();
            }
            // Skip the remaining indices.
            m_nextIndex.store(m_count, std::memory_order_relaxed);
        }
    }
}

}  // namespace starcore
