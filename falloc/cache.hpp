/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <atomic> // atomic
#include <memory> // unique_ptr

#include <falloc/impexp.hpp>

namespace falloc {

/// Returns trash in caches into slabs.
FALLOC_IMPEXP bool maintain_all_caches() noexcept;
/// Returns trash in caches into slabs and delete all free slabs.
FALLOC_IMPEXP bool clear_all_caches() noexcept;
FALLOC_IMPEXP void new_handler();

namespace detail {

struct cache_global;
struct cache_local;

FALLOC_IMPEXP cache_global * make_cache_global();
FALLOC_IMPEXP void free_cache_global(cache_global *);

// object of this class must be used only in those thread,
// in which it was created.
class FALLOC_IMPEXP cache {
public:
    // ctor and dtor must be called in the same thread for properly deletion
    // from maintain list.
    cache(detail::cache_global *global, unsigned stat_interval, size_t align,
            size_t object_size);
    ~cache();

    void * alloc() noexcept;
    void free(void *obj) noexcept;

    bool maintain_global() noexcept;
    bool maintain_local() noexcept;
    bool clear_local() noexcept;

private:
    detail::cache_global *global;
    std::unique_ptr<detail::cache_local> local;
};

} // namespace detail

template<typename Object, typename Tag = void>
class object_cache {
public:
    static Object * alloc() { return reinterpret_cast<Object *>(impl.alloc()); }

    static void free(Object *p) { impl.free(p); }

    static void maintain()
    {
        impl.maintain_local();
        impl.maintain_global();
    }

    static void clear()
    {
        impl.clear_local();
        impl.maintain_global();
    }

    struct set_stat_interval {
        set_stat_interval(unsigned stat_interval)
        {
            object_cache::stat_interval.store(stat_interval);
        }
    };

private:
    static std::unique_ptr<
        detail::cache_global,
        void (*)(detail::cache_global *)> global;

    static std::atomic<unsigned> stat_interval;

    thread_local static detail::cache impl;
};

template<typename Object, typename Tag>
std::unique_ptr<detail::cache_global, void (*)(detail::cache_global *)>
object_cache<Object, Tag>::global(
        detail::make_cache_global(), detail::free_cache_global);

template<typename Object, typename Tag>
std::atomic<unsigned> object_cache<Object, Tag>::stat_interval(0x10000000);

template<typename Object, typename Tag>
thread_local detail::cache object_cache<Object, Tag>::impl(
    global.get(), stat_interval.load(), alignof(Object), sizeof(Object));

} // namespace falloc
