#pragma once

namespace falloc {

// In all contemporary compilers memory_order_consume promoted to
// memory_order_acquire.
// But for all CPU other than DEC Alpha 21264 loads with memory_order_consume
// is the same as memory_order_relaxed.

template<typename T>
[[gnu::always_inline]]
inline T load_consume(std::atomic<T> const &x)
{
    // Wrong for DEC Alpha 21264. But OK for others and contemporary processors.
    // Because relaxed and consume are same for they if supposing that compiler
    // wouldn't break dependency chain (true for pointer dereference).
    T res = x.load(std::memory_order_relaxed);
    asm volatile("" ::: "memory");
    return res;
}

template<typename T, typename U>
[[gnu::always_inline]]
inline T exchange_consume(std::atomic<T> &x, U &&desired)
{
    // Wrong for DEC Alpha 21264. But OK for others and contemporary processors.
    // Because relaxed and consume are same for they if supposing that compiler
    // wouldn't break dependency chain (true for pointer dereference).
    T res = x.exchange(std::forward<U>(desired), std::memory_order_relaxed);
    asm volatile("" ::: "memory");
    return res;
}

} // namespace falloc
