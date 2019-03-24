/*
 * Copyright (C) Andrey Pikas
 */
#include <falloc/functions.hpp>
#include <falloc/gp_allocator.hpp>
#include <falloc/pool.hpp>

#include <atomic> // atomic
#include <cassert> // assert
#include <cstring> // memset
#include <memory> // align
#include <mutex> // lock_guard

#include <sys/mman.h>

#include "consume_ordering.hpp"
#include "spinlock.hpp"

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

namespace falloc {

constexpr long PAGESIZE = 4096;
constexpr long HUGEPAGESIZE = 2 << 20;
constexpr uintptr_t PAGESTART_MASK = ~(uintptr_t)PAGESIZE + 1;
constexpr uintptr_t HUGEPAGESTART_MASK = ~(uintptr_t)HUGEPAGESIZE + 1;


[[gnu::always_inline]]
inline slab_header * slab_of_obj(void *obj)
{
    return reinterpret_cast<slab_header *>((uintptr_t)obj & HUGEPAGESTART_MASK);
}

[[gnu::always_inline]]
inline void * page_of_obj(void *obj)
{
    return reinterpret_cast<void *>((uintptr_t)obj & PAGESTART_MASK);
}


//
// list_node 
//

template<typename T>
inline list_node<T>::list_node() : next(this), prev(this) {}

template<typename T>
inline T * list_node<T>::get()
{
    return static_cast<T *>(this);
}

template<typename T>
inline void list_node<T>::append(list_node *other)
{
    other->next = next;
    next->prev = other;
    next = other;
    other->prev = this;
}

template<typename T>
inline void list_node<T>::remove()
{
    next->prev = prev;
    prev->next = next;
    next = prev = this;
}


//
// global_trash
//

[[maybe_unused]]
global_trash::global_trash() noexcept : free_list_(nullptr) {}

global_trash::global_trash(already_initialized) noexcept {}

void global_trash::add_region(void *p, size_t size) noexcept
{
    reinterpret_cast<size_t *>(p)[1] = size;
    *reinterpret_cast<void **>(p) = free_list_.load(std::memory_order_relaxed);
    while (!free_list_.compare_exchange_weak(
                *reinterpret_cast<void **>(p), p,
                std::memory_order_release, std::memory_order_relaxed)) {}
}

bool global_trash::clean() noexcept
{
    if (!free_list_.load(std::memory_order_relaxed))
        return false;

    bool result = false;
    for (bool unmaped = true; unmaped;) {
        unmaped = false;
        void *p = exchange_consume(free_list_, nullptr);
        while (p) {
            size_t *r = reinterpret_cast<size_t *>(p);
            p = *reinterpret_cast<void **>(p);
            if (munmap(r, r[1]) == 0)
                result = unmaped = true;
            else
                add_region(r, r[1]);
        }
    }
    return result;
}


class global_trash global_trash(global_trash::already_initialized{});


///
/// slab_header
///

struct slab_header {
    // obj_size used to distinguish slab_header from pages of large allocator
    unsigned obj_size;
    int used_cnt = 0;
    void *free = nullptr;
    slab_header *next = nullptr;
    slab_header *prev = nullptr;
    spinlock owner_lock;
    std::atomic<void *> owner = {nullptr};

    bool init(unsigned size, unsigned alignment);
    inline void * alloc();
    inline void put_free(void *obj);
};

bool slab_header::init(unsigned size, unsigned alignment)
{
    size_t real_size = size;
    // free object will contain pointer to the next free at its begining.
    if (size < sizeof(void *))
        size = sizeof(void *);

    char *p = reinterpret_cast<char *>(this) + sizeof(*this);
    char *end = reinterpret_cast<char *>(this) + HUGEPAGESIZE;
    char *page = reinterpret_cast<char *>(page_of_obj(p));
    void **last = &free;
    while (p < end) {
        *reinterpret_cast<unsigned *>(page) = real_size; // assign to obj_size
        char *page_end = page + PAGESIZE;
        size_t space = page_end - p;
        if (!std::align(alignment, size, reinterpret_cast<void *&>(p), space))
            return false;

        char *next = p + size;
        do {
            *last = p;
            last = reinterpret_cast<void **>(p);
            p = next;
            next += size;
        }
        while (next <= page_end);

        page = reinterpret_cast<char *>(page_of_obj(next));
        p = page + sizeof(unsigned);
    }

    *last = nullptr;

    // Check that slab has space for at least two objects.
    // This property used when moving slab from full to partial (not to free).
    return free && *reinterpret_cast<void **>(free);
}

inline void * slab_header::alloc()
{
    if (free) {
        void *result = free;
        free = *reinterpret_cast<void **>(result);
        ++used_cnt;
        return result;
    }
    return nullptr;
}

inline void slab_header::put_free(void *obj)
{
    *reinterpret_cast<void **>(obj) = free;
    free = obj;
    --used_cnt;
}

int create_slabs(unsigned object_size, unsigned alignment, void *owner,
        slab_header **list)
{
    int cnt;
#ifndef MAP_HUGE_2MB
    constexpr int MAP_HUGE_2MB = (21 << MAP_HUGE_SHIFT);
#endif
    char *p = (char *)mmap(nullptr, HUGEPAGESIZE, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
    if (p == MAP_FAILED || !p) { // Allow zero page to leak.
        p = (char *)mmap(nullptr, 2 * HUGEPAGESIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED || !p) // Allow zero page to leak.
            return 0;
        // Align region of small pages by huge page size.
        size_t right_pad_size = uintptr_t(p) & ~HUGEPAGESTART_MASK;
        if (right_pad_size) {
            size_t left_pad_size = HUGEPAGESIZE - right_pad_size;
            if (munmap(p, left_pad_size))
                global_trash.add_region(p, left_pad_size);
            p += left_pad_size;
            if (munmap(p + HUGEPAGESIZE, right_pad_size))
                global_trash.add_region(p + HUGEPAGESIZE, right_pad_size);
            cnt = 1;
        }
        else
            cnt = 2;
    }
    else
        cnt = 1;

    int ret_cnt = 0;
    for (int i = 0, offs = 0; i < cnt; ++i, offs += HUGEPAGESIZE) {
        slab_header *slab = reinterpret_cast<slab_header *>(p + offs);
        new (slab) slab_header;
        if (slab->init(object_size, alignment)) {
            slab->owner.store(owner, std::memory_order_relaxed);
            slab->next = *list;
            *list = slab;
            ++ret_cnt;
        }
        else if (munmap(p + offs, HUGEPAGESIZE))
            global_trash.add_region(p + offs, HUGEPAGESIZE);
    }
    return ret_cnt;
}

bool delete_slab(slab_header *slab)
{
    // At this moment slab can't be used from other threads.
    return !munmap(slab, HUGEPAGESIZE);
}

///
/// pool_global
///

pool_global & pool_global::instance()
{
    static pool_global inst;
    return inst;
}

pool_global::~pool_global()
{
    maintain();
}

bool pool_global::maintain()
{
    if (!trash.load(std::memory_order_relaxed))
        return false;
    if (clean_lock.test_and_set(std::memory_order_acquire))
        return false;
    bool result = false;
    void *trash_mtn = exchange_consume(trash, nullptr);
    while (trash_mtn) {
        void *trash_del = trash_mtn;
        trash_mtn = *reinterpret_cast<void **>(trash_mtn);
        slab_header *slab = slab_of_obj(trash_del);
        slab->put_free(trash_del);
        if (!slab->used_cnt) {
            if (delete_slab(slab))
                result = true;
            else {
                slab->next = free_slabs;
                free_slabs = slab;
            }
        }
    }

    for (bool slabs_deleted = true; slabs_deleted;) {
        slabs_deleted = false;
        // munmap may fail if unmaping should split mapped region into two ones
        // and make number of mapped regions greater then vm.max_map_count.
        // In this case try unmap in different orders. May be we will find such
        // an order in which regions unmapped from ends of mapped regions.
        slab_header *not_deleted = nullptr;
        while (free_slabs) {
            slab_header *p = free_slabs;
            free_slabs = free_slabs->next;
            if (delete_slab(p))
                slabs_deleted = result = true;
            else {
                p->next = not_deleted;
                not_deleted = p;
            }
        }
        free_slabs = not_deleted;
    }


    // Unlock only after loop to protect slabs of objects from trash,
    // not only trash pointer itself. This slabs are owned by pool_global,
    // but pool_global::maintain may be called from different threads.
    // After reseting trash to nullptr it may be reseted from other thread again
    // to non-null with object belonging to the same slab.
    clean_lock.clear(std::memory_order_release);
    return result;
}


///
/// pool_local
///

namespace {

thread_local list_node<pool_local> local_maintain_list;

} // namespace

pool_local::pool_local(unsigned object_size, unsigned alignment,
        unsigned stat_interval)
    : alignment(alignment), stat_interval(stat_interval), timer(stat_interval),
    object_size(object_size)
{
    local_maintain_list.append(this);
}

pool_local::pool_local() : pool_local(0, 0, 0x10000000) {}

void pool_local::init(unsigned object_size, unsigned alignment,
        unsigned stat_interval)
{
    assert(!all_slabs_cnt);
    this->alignment = alignment;
    timer = this->stat_interval = stat_interval;
    this->object_size = object_size;
}

pool_local::~pool_local()
{
    // Move one_hold_partial and trash objects to slabs
    // while slabs owned by this local pool. Delete all free slabs.
    maintain(0);

    auto list_to_global = [](slab_header *slab) {
        while (slab) {
            // After unlock slab may be deleted. So use it before.
            slab_header *next = slab->next;
            slab->owner_lock.lock();
            slab->owner.store(&pool_global::instance(),
                    std::memory_order_relaxed);
            slab->owner_lock.unlock();
            slab = next;
        }
    };
    list_to_global(partial_slabs);
    list_to_global(full_slabs);

    // All slabs now owned by global pool.
    // Local trash is unchangeable after moving all slabs to global.
    void *first_trash = load_consume(trash);
    if (first_trash) {
        void *last_trash = first_trash;
        while (*reinterpret_cast<void **>(last_trash))
            last_trash = *reinterpret_cast<void **>(last_trash);
        pool_global &global = pool_global::instance();
        // Note: the below use is not thread-safe in at least
        // GCC prior to 4.8.3 (bug 60272), clang prior to 2014-05-05 (bug 18899)
        // MSVC prior to 2014-03-17 (bug 819819). So update your compiler.
        *reinterpret_cast<void **>(last_trash) =
            global.trash.load(std::memory_order_relaxed);
        while (!global.trash.compare_exchange_weak(
                    *reinterpret_cast<void **>(last_trash), first_trash,
                    std::memory_order_release, std::memory_order_relaxed)) {}
    }

    remove(); // remove from local_maintain_list
}

inline void pool_local::push(slab_header **list, slab_header *slab)
{
    slab->next = *list;
    slab->prev = nullptr;
    if (slab->next)
        slab->next->prev = slab;
    *list = slab;
}

inline void pool_local::remove_from_list(slab_header **list, slab_header *slab)
{
    if (slab->next)
        slab->next->prev = slab->prev;
    if (slab->prev)
        slab->prev->next = slab->next;
    if (*list == slab)
        *list = slab->next;
}

inline void pool_local::return_obj_to_slab(void *obj, slab_header *slab)
{
    if (unlikely(!slab->free)) {
        // slab in full list now. Move it to partial.
        remove_from_list(&full_slabs, slab);
        push(&partial_slabs, slab);
    }
    else if (unlikely(slab->used_cnt == 1)) {
        // slab will be free after putting object. Move it from partial to free.
        remove_from_list(&partial_slabs, slab);
        slab->next = free_slabs;
        free_slabs = slab;
        --used_slabs_cnt;
    }
    slab->put_free(obj);
}

inline void * pool_local::alloc_inline() noexcept
{
    --timer;
    if (partial_slabs) {
        slab_header *slab = partial_slabs;
        void *result = slab->alloc();
        if (slab->free) {
            if (timer)
                return result;
            maintain(slabs_limit);
            return result;
        }
        partial_slabs = slab->next;
        if (partial_slabs)
            partial_slabs->prev = nullptr;
        push(&full_slabs, slab);
        if (timer)
            return result;
        maintain(slabs_limit);
        return result;
    }
    else {
        if (!free_slabs) {
            all_slabs_cnt += create_slabs(object_size, alignment, this,
                    &free_slabs);
            if (!free_slabs) {
                if (!timer)
                    maintain(slabs_limit);
                return nullptr;
            }
        }
        slab_header *slab = free_slabs;
        free_slabs = slab->next;
        if (++used_slabs_cnt > stat_max_used_cnt)
            stat_max_used_cnt = used_slabs_cnt;
        push(&partial_slabs, slab);
        if (!timer)
            maintain(slabs_limit);
        return slab->alloc();
    }
}

void * pool_local::alloc() noexcept { return alloc_inline(); }

inline void pool_local::free_inline(void *obj) noexcept
{
    slab_header *slab = slab_of_obj(obj);
    if (slab->owner.load(std::memory_order_relaxed) == this) {
        if (one_hold_partial)
            return return_obj_to_slab(obj, slab);
        else
            return void(one_hold_partial = obj);
    }

    std::lock_guard<spinlock> lock(slab->owner_lock);
    void *owner = slab->owner.load(std::memory_order_relaxed);
    pool_global *global = &pool_global::instance();
    std::atomic<void *> &trash = (owner == global
        ? global->trash
        : reinterpret_cast<pool_local *>(owner)->trash);
    // Note: the below use is not thread-safe in at least
    // GCC prior to 4.8.3 (bug 60272), clang prior to 2014-05-05 (bug 18899)
    // MSVC prior to 2014-03-17 (bug 819819). So update your compiler.
    *reinterpret_cast<void **>(obj) = trash.load(std::memory_order_relaxed);
    while (!trash.compare_exchange_weak(*reinterpret_cast<void **>(obj), obj,
                std::memory_order_release, std::memory_order_relaxed)) {}

}

void pool_local::free(void *obj) noexcept { return free_inline(obj); }

bool pool_local::maintain(size_t limit)
{
    bool result = false;
    if (one_hold_partial) {
        return_obj_to_slab(one_hold_partial, slab_of_obj(one_hold_partial));
        one_hold_partial = nullptr;
        result = true;
    }

    if (trash.load(std::memory_order_relaxed)) {
        void *trash_mtn = exchange_consume(trash, nullptr);
        while (trash_mtn) {
            void *trash_rec = trash_mtn;
            trash_mtn = *reinterpret_cast<void **>(trash_mtn);
            // Objects in trash lays in slabs of this pool_local.
            return_obj_to_slab(trash_rec, slab_of_obj(trash_rec));
            result = true;
        }
    }

    for (bool slabs_deleted = true; slabs_deleted;) {
        slabs_deleted = false;
        // munmap may fail if unmaping should split mapped region into two ones
        // and make number of mapped regions greater then vm.max_map_count.
        // In this case try unmap in different orders. May be we will find such
        // an order in which regions unmapped from ends of mapped regions.
        slab_header *not_deleted_first = nullptr, *not_deleted_last = nullptr;
        while (all_slabs_cnt > limit && free_slabs) {
            slab_header *p = free_slabs;
            free_slabs = free_slabs->next;
            if (delete_slab(p)) {
                --all_slabs_cnt;
                slabs_deleted = result = true;
            }
            else {
                p->next = not_deleted_first;
                not_deleted_first = p;
                if (!not_deleted_last)
                    not_deleted_last = p;
            }
        }
        if (not_deleted_last) {
            not_deleted_last->next = free_slabs;
            free_slabs = not_deleted_first;
        }
    }

    if (!timer) {
        slabs_limit = stat_max_used_cnt;
        stat_max_used_cnt = used_slabs_cnt;
        timer = stat_interval;
    }

    result |= pool_global::instance().maintain();
    return result;
}


bool maintain_all_pools() noexcept
{
    bool result = false;
    for (list_node<pool_local> *e =
            &local_maintain_list, *l = e->next; l != e; l = l->next) {
        pool_local *c = l->get();
        result |= c->maintain(c->slabs_limit);
    }

    result |= pool_global::instance().maintain();
    result |= global_trash.clean();
    return result;
}

bool clear_all_pools() noexcept
{
    bool result = false;
    for (list_node<pool_local> *e =
            &local_maintain_list, *l = e->next; l != e; l = l->next) {
        pool_local *c = l->get();
        result |= c->maintain(0);
    }

    result |= pool_global::instance().maintain();
    result |= global_trash.clean();
    return result;
}


//
// gp_allocator_local
//

gp_allocator_local::gp_allocator_local(size_t stat_interval) noexcept
{
    for (size_t sz = MAX_POOLED_SIZE; sz > 0; --sz) {
        size_t idx = size_to_idx(sz);
        if (pools_[idx].object_size)
            continue;
        pools_[idx].init(sz, std::min(64U, (1U << log2floor(sz))),
                stat_interval);
    }
}

[[gnu::always_inline]]
inline void * gp_allocator_local::allocate_inline(size_t size) noexcept
{
    if (likely(likely(size) && likely(size <= MAX_POOLED_SIZE)))
        return pools_[size_to_idx(size)].alloc_inline();
    else if (size) {
        void *p = mmap(nullptr, size + 64,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            return nullptr;
        reinterpret_cast<unsigned *>(p)[0] = MAX_POOLED_SIZE + 1;
        reinterpret_cast<size_t *>(p)[1] = size + 64;
        return reinterpret_cast<char *>(p) + 64;
    }
    else
        return nullptr;
}

void * gp_allocator_local::allocate(size_t size) noexcept
{
    return allocate_inline(size);
}

void * gp_allocator_local::allocate_zeroed(size_t size) noexcept
{
    void *p = allocate_inline(size);
    if (p && size && size <= MAX_POOLED_SIZE)
        memset(p, 0, size);
    return p;
}

[[gnu::always_inline]]
inline void gp_allocator_local::free_inline(void *p) noexcept
{
    void *page_start = page_of_obj(p);
    unsigned objsize = *reinterpret_cast<unsigned *>(page_start);
    if (likely(objsize <= MAX_POOLED_SIZE)) {
        pools_[size_to_idx(objsize)].free_inline(p);
        return;
    }
    else {
        size_t size = reinterpret_cast<size_t *>(page_start)[1];
        if (munmap(page_start, size) != 0)
            global_trash.add_region(page_start, size);
        global_trash.clean();
    }
}

void * gp_allocator_local::resize(void *p, size_t nsize) noexcept
{
    if (unlikely(!p))
        return allocate_inline(nsize);

    void *page_start = page_of_obj(p);
    unsigned objsize = *reinterpret_cast<unsigned *>(page_start);
    size_t size;
    if (objsize <= MAX_POOLED_SIZE) {
        unsigned old_idx = size_to_idx(objsize);
        unsigned new_idx = size_to_idx(static_cast<unsigned>(nsize));
        if (old_idx == new_idx)
            return p;
        size = objsize;
    }
    else {
        size = reinterpret_cast<size_t *>(page_start)[1];
        size_t old_size = (size + PAGESIZE - 1) & PAGESTART_MASK;
        size_t new_size = (64 + nsize + PAGESIZE - 1) & PAGESTART_MASK;
        if (new_size <= old_size) {
            size_t free_size = size - new_size;
            if (free_size) {
                void *fp = reinterpret_cast<char *>(page_start) + new_size;
                if (munmap(fp, free_size) != 0)
                    global_trash.add_region(fp, free_size);
                global_trash.clean();
            }
            if (!new_size)
                return nullptr;
            reinterpret_cast<size_t *>(page_start)[1] = nsize + 64;
            return p;
        }
    }

    if (void *np = allocate_inline(nsize)) {
        memcpy(np, p, std::min(size, nsize));
        free_inline(p);
        return np;
    }
    return nullptr;
}

void gp_allocator_local::free(void *p) noexcept { return free_inline(p); }

static thread_local gp_allocator_local default_gp_allocator;


void * allocate(size_t size) noexcept
{
    return default_gp_allocator.allocate_inline(size);
}

void free(void *p) noexcept
{
    return default_gp_allocator.free_inline(p);
}

} // namespace falloc

#ifdef FALLOC_STDAPI
extern "C" {

FALLOC_IMPEXP void * malloc(size_t size) noexcept
{
    return falloc::default_gp_allocator.allocate_inline(size);
}

FALLOC_IMPEXP void * calloc(size_t num, size_t size) noexcept
{
    return falloc::default_gp_allocator.allocate_zeroed(size * num);
}

FALLOC_IMPEXP void free(void *p) noexcept
{
    if (p)
        falloc::default_gp_allocator.free_inline(p);
}

FALLOC_IMPEXP void * realloc(void *p, size_t size)
{
    return falloc::default_gp_allocator.resize(p, size);
}

FALLOC_IMPEXP void * aligned_alloc(size_t alignment, size_t size)
{
    return falloc::default_gp_allocator.allocate(std::max(size, alignment));
}

} // extern "C"
#endif
