#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace starcore
{

/**
 * @brief A fixed pool of worker threads that runs blocking parallel-for jobs.
 *
 * Portable replacement for `std::for_each(std::execution::par, ...)`, which
 * libstdc++ runs sequentially unless linked against TBB. The calling thread
 * participates in every job. Jobs must not call parallelFor() recursively.
 */
class ParallelExecutor
{
public:
    /** @param threadCount Total threads including the caller; 0 = hardware. */
    explicit ParallelExecutor(unsigned threadCount = 0);
    ~ParallelExecutor();

    ParallelExecutor(const ParallelExecutor&) = delete;
    auto operator=(const ParallelExecutor&) -> ParallelExecutor& = delete;

    /** @brief Number of threads that execute work (workers + caller). */
    [[nodiscard]] auto threadCount() const noexcept -> unsigned
    {
        return static_cast<unsigned>(m_workers.size()) + 1;
    }

    /**
     * @brief Invoke `fn(i)` for every i in [0, count) and wait for all calls.
     *        The first exception thrown by `fn` is rethrown here.
     */
    template <typename Fn>
        requires std::is_invocable_v<Fn&, std::size_t>
    void parallelFor(std::size_t count, Fn&& fn)
    {
        run(count,
            [](void* context, std::size_t index) {
                (*static_cast<std::remove_reference_t<Fn>*>(context))(index);
            },
            static_cast<void*>(&fn));
    }

private:
    using InvokeFn = void (*)(void*, std::size_t);

    void run(std::size_t count, InvokeFn invoke, void* context);
    void workerLoop(const std::stop_token& stopToken);
    void processIndices();

    std::vector<std::jthread> m_workers;

    std::mutex m_mutex;
    std::condition_variable_any m_wakeWorkers;
    std::condition_variable m_jobDone;
    std::size_t m_generation = 0;   // Guarded by m_mutex.
    unsigned m_activeWorkers = 0;   // Guarded by m_mutex.
    std::exception_ptr m_exception; // Guarded by m_mutex.

    // Current job (written before m_generation is bumped).
    InvokeFn m_invoke = nullptr;
    void* m_context = nullptr;
    std::size_t m_count = 0;
    std::atomic<std::size_t> m_nextIndex{0};
};

}  // namespace starcore
