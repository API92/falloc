#pragma once

#include <atomic>
#include <thread>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)

#include <emmintrin.h>
#define PAUSE _mm_pause()

#endif

namespace falloc {

[[gnu::always_inline]]
inline void yield(unsigned k) noexcept
{
#if defined(PAUSE)
    if (k < 4) {}
    else if (k < 32)
        PAUSE;
#else
    if (k < 32) {}
#endif
    else
        std::this_thread::yield();
}
 

class spinlock {
public:
    bool try_lock() noexcept
    {
        return !locked_.test_and_set(std::memory_order_acquire);
    }

    void lock() noexcept
    {
        for (unsigned i = 0; !try_lock(); ++i)
            yield(i);
    }

    void unlock() noexcept
    {
        locked_.clear(std::memory_order_release);
    }

private:
    std::atomic_flag locked_ = ATOMIC_FLAG_INIT;
};

} // namespace falloc
