/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <falloc/impexp.hpp>
#include <falloc/pool.hpp>

namespace falloc {

template<typename Object, typename Tag = void>
class object_pool {
public:
    [[gnu::always_inline]]
    static inline Object * alloc() noexcept
    {
        return reinterpret_cast<Object *>(impl().alloc());
    }

    [[gnu::always_inline]]
    static Object * alloc_with_new_handler() noexcept
    {
        return reinterpret_cast<Object *>(impl().alloc_with_new_handler());
    }

    [[gnu::always_inline]]
    static inline void free(Object *p) noexcept
    {
        impl().free(p);
    }

    static void maintain() noexcept
    {
        impl().maintain(impl().slabs_limit);
    }

    static void clear() noexcept
    {
        impl().maintain(0);
    }

    static void set_stat_interval(unsigned x) noexcept
    {
        stat_interval = x;
        impl().stat_interval = x;
    }

    struct stat_interval_setter {
        stat_interval_setter(unsigned stat_interval) noexcept
        {
            set_stat_interval(stat_interval);
        }
    };

private:
    [[gnu::always_inline]]
    static inline pool_local & impl() noexcept
    {
        static thread_local pool_local impl_(sizeof(Object), alignof(Object),
                stat_interval);
        return impl_;
    }

    static unsigned stat_interval;
};


template<typename Object, typename Tag>
unsigned object_pool<Object, Tag>::stat_interval = 0x10000000;

} // namespace falloc

