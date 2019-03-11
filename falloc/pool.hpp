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
constexpr long HUGEPAGESIZE = 2 << 20;
constexpr uintptr_t HUGEPAGESTART_MASK = ~(uintptr_t)HUGEPAGESIZE + 1;

struct slab_header;

[[gnu::always_inline]]
inline slab_header * slab_of_obj(void *obj)
{
    return reinterpret_cast<slab_header *>((uintptr_t)obj & HUGEPAGESTART_MASK);
}

// Storage for memory regions that cannot be unmaped.
class global_trash {
public:
    struct already_initialized {};

    global_trash() noexcept;
    global_trash(already_initialized) noexcept;

    void add_region(void *p, size_t size) noexcept;
    bool clean() noexcept;

private:
    std::atomic<void *> free_list_;
};

extern class global_trash global_trash;


class pool_global {
public:
    std::atomic<void *> trash = {nullptr};
    // Singly linked list of slabs which can not be munmaped.
    slab_header *free_slabs = nullptr;
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
    inline list_node();

    inline T * get();
    inline void append(list_node *other);
    inline void remove();

    list_node *next;
    list_node *prev;
};

// Used in single thread (except trash field).
struct FALLOC_IMPEXP pool_local : list_node<pool_local> {

    size_t all_slabs_cnt = 0; // number of slabs in all lists #cold
    size_t slabs_limit = ~0ULL; // maximum number of slabs (soft limit) #cold
    unsigned alignment; // #cold
    unsigned stat_interval; // #cold

    // list for objects, freed from foreign threads
    std::atomic<void *> trash = {nullptr}; // #invalidates cache line

    alignas(64) // one cache line for hot and warm members
    // one object, that holds one slab in partial list, to not waste time on
    // moving slab between free and partial lists.
    void *one_hold_partial = nullptr; // #hot

    unsigned timer; // #hot
    unsigned object_size; // size of pooled objects #cold

    // this lists used only in owner thread
    slab_header *partial_slabs = nullptr; // double linked #hot
    slab_header *free_slabs = nullptr; // singly linked #warm
    slab_header *full_slabs = nullptr; // double linked #warm

    size_t used_slabs_cnt = 0; // number of slabs in partial and full lists #warm
    size_t stat_max_used_cnt = 0; // maximum of used_slabs_cnt within interval #warm

    pool_local(unsigned object_size, unsigned alignment, unsigned stat_interval);
    ~pool_local();
    pool_local();
    void init(unsigned object_size, unsigned alignment, unsigned stat_interval);

    // push slab into the begining of the list
    inline static void push(slab_header **list, slab_header *slab);
    // remove slab from double linked list
    inline static void remove_from_list(slab_header **list, slab_header *slab);
    // put obj to slab's free list and move slab between full/partial/free lists
    inline void return_obj_to_slab(void *obj, slab_header *slab);
    // allocate one object.
    FALLOC_IMPEXP void * alloc() noexcept;
    inline void * alloc_inline() noexcept;
    // free object obj if it's owned by this pool.
    // Else put it into owner's trash.
    FALLOC_IMPEXP void free(void *obj) noexcept;
    inline void free_inline(void *obj) noexcept;
    // return objects from trash to their slabs, leave maximum free_slabs_limit
    // slabs in free list.
    FALLOC_IMPEXP bool maintain(size_t slabs_limit);

    // Tries to allocate. Else calls std::get_new_handler() and tries again.
    FALLOC_IMPEXP void * alloc_with_new_handler() noexcept;
};


} // namespace falloc
