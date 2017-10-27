/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <atomic> // atomic
#include <cstddef> // size_t
#include <cstdint> // uintptr_t

#include <falloc/impexp.hpp>

namespace falloc {

constexpr long PAGESIZE = 4096;
constexpr uintptr_t PAGESTART_MASK = ~(uintptr_t)PAGESIZE + 1;

struct slab_header;

inline static slab_header * slab_of_obj(void *obj)
{
    return reinterpret_cast<slab_header *>((uintptr_t)obj & PAGESTART_MASK);
}

/// Returns trash in pools into slabs.
FALLOC_IMPEXP bool maintain_all_pools() noexcept;
/// Returns trash in pools into slabs and delete all free slabs.
FALLOC_IMPEXP bool clear_all_pools() noexcept;
FALLOC_IMPEXP void new_handler();


class pool_global {
public:
    std::atomic<void *> trash = {nullptr};
    std::atomic_flag clean_lock = ATOMIC_FLAG_INIT;

    static pool_global & instance();
    ~pool_global();
    bool maintain();

private:
    pool_global() = default;
    pool_global(pool_global const &) = delete;
    pool_global(pool_global &&) = delete;
    void operator = (pool_global const &) = delete;
    void operator = (pool_global &&) = delete;
};


template<typename T>
struct list_node {
    list_node() : next(this), prev(this) {}

    T * get()
    {
        return static_cast<T *>(this);
    }

    void append(list_node *other)
    {
        other->next = next;
        next->prev = other;
        next = other;
        other->prev = this;
    }

    void remove()
    {
        next->prev = prev;
        prev->next = next;
        next = prev = this;
    }

    list_node *next;
    list_node *prev;
};

// Used in single thread (except trash field).
struct pool_local : list_node<pool_local> {
    // one object, that holds one slab in partial list, to not waste time on
    // moving slab between free and partial lists.
    void *one_hold_partial = nullptr;

    // this lists used only in owner thread
    slab_header *partial_slabs = nullptr; // double linked
    slab_header *free_slabs = nullptr; // singly linked
    slab_header *full_slabs = nullptr; // double linked
    size_t used_slabs_cnt = 0; // number of slabs in partial and full lists
    size_t all_slabs_cnt = 0; // number of slabs in all lists
    size_t stat_max_used_cnt = 0; // maximum of used_slabs_cnt within interval

    unsigned timer;
    unsigned stat_interval;

    size_t object_size; // size of pooled objects
    size_t slabs_limit = ~0ULL; // maximum number of slabs (soft limit)

    // list for objects, freed from foreign threads
    std::atomic<void *> trash = {nullptr};

    pool_local(size_t object_size, unsigned stat_interval);
    ~pool_local();
    pool_local();
    void init(size_t object_size, unsigned stat_interval);

    // push slab into the begining of the list
    inline static void push(slab_header **list, slab_header *slab);
    // remove slab from double linked list
    inline static void remove_from_list(slab_header **list, slab_header *slab);
    // put obj to slab's free list and move slab between full/partial/free lists
    inline void return_obj_to_slab(void *obj, slab_header *slab);
    // allocate one object.
    void * alloc();
    // free object obj if it's owned by this pool.
    // Else put it into owner's trash.
    void free(void *obj);
    // return objects from trash to their slabs, leave maximum free_slabs_limit
    // slabs in free list.
    bool maintain(size_t slabs_limit);

    // Tries to allocate. Else calls std::get_new_handler() and tries again.
    void * alloc_with_new_handler() noexcept;
};


} // namespace falloc
