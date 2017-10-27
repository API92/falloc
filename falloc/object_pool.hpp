/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <atomic> // atomic
#include <memory> // unique_ptr

#include <falloc/impexp.hpp>

namespace falloc {

struct pool_local;

// object of this class must be used only in those thread,
// in which it was created.
class FALLOC_IMPEXP object_pool_impl {
public:
    // ctor and dtor must be called in the same thread for properly deletion
    // from maintain list.
    object_pool_impl(size_t object_size, unsigned stat_interval);
    ~object_pool_impl();

    void * alloc() noexcept;
    void free(void *obj) noexcept;

    bool maintain_local() noexcept;
    bool clear_local() noexcept;

    // Tries to allocate. Else calls std::get_new_handler() and tries again.
    void * alloc_with_new_handler() noexcept;

private:
    std::unique_ptr<pool_local> local;
};

template<typename Object, typename Tag = void>
class object_pool {
public:
    static Object * alloc()
    {
        return reinterpret_cast<Object *>(impl().alloc());
    }

    static Object * alloc_with_new_handler() noexcept
    {
        return reinterpret_cast<Object *>(impl().alloc_with_new_handler());
    }

    static void free(Object *p) { impl().free(p); }

    static void maintain()
    {
        impl().maintain_local();
    }

    static void clear()
    {
        impl().clear_local();
    }

    struct set_stat_interval {
        set_stat_interval(unsigned stat_interval)
        {
            object_pool::stat_interval = stat_interval;
        }
    };

private:
    static object_pool_impl & impl()
    {
        static thread_local object_pool_impl impl_inst(
            sizeof(Object), stat_interval);
        return impl_inst;
    }

    static unsigned stat_interval;
};

template<typename Object, typename Tag>
unsigned object_pool<Object, Tag>::stat_interval(0x10000000);

} // namespace falloc

