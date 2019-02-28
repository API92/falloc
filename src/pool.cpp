/*
 * Copyright (C) Andrey Pikas
 */

#include <falloc/pool.hpp>

#include <atomic> // atomic
#include <cassert> // assert
#include <memory> // align

#include <boost/smart_ptr/detail/spinlock.hpp>
#include <sys/mman.h>

#include "consume_ordering.hpp"

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

namespace falloc {

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
    boost::detail::spinlock owner_lock;
    std::atomic<void *> owner = {nullptr};

    void init(unsigned size);
    inline void * alloc();
    inline void put_free(void *obj);
};

void slab_header::init(unsigned size)
{
    obj_size = size;
    // free object will contain pointer to the next free at its begining.
    if (size < sizeof(void *))
        size = sizeof(void *);

    char *p = reinterpret_cast<char *>(this) + sizeof(*this);
    char *end = reinterpret_cast<char *>(this) + PAGESIZE;
    size_t space = end - p;
    if (!std::align(size, size, reinterpret_cast<void *&>(p), space))
        return;

    char *next = p + size;
    do {
        *reinterpret_cast<void **>(p) = free;
        free = p;
        p = next;
        next += size;
    }
    while (next < end);

    // Check that slab has space for at least two objects.
    // This property used when moving slab from full to partial (not to free).
    assert(free && *reinterpret_cast<void **>(free));
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

int create_slabs(unsigned object_size, void *owner, slab_header **list)
{
    size_t cnt = 4;
    char *p = nullptr;
    for (; cnt; --cnt) {
        p = (char *)mmap(nullptr, cnt * PAGESIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED || !p) // Allow zero page to leak.
            continue;
        break;
    }
    if (!cnt)
        return 0;

    for (size_t i = 0, offs = 0; i < cnt; ++i, offs += PAGESIZE) {
        slab_header *slab = reinterpret_cast<slab_header *>(p + offs);
        new (slab) slab_header;
        slab->init(object_size);
        slab->owner.store(owner, std::memory_order_relaxed);
        slab->next = *list;
        *list = slab;
    }
    return cnt;
}

bool delete_slab(slab_header *slab)
{
    // At this moment slab can't be used from other threads.
    return !munmap(slab, PAGESIZE);
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

pool_local::pool_local(unsigned object_size, unsigned stat_interval)
    : stat_interval(stat_interval), timer(stat_interval),
    object_size(object_size)
{
    local_maintain_list.append(this);
}

pool_local::pool_local() : pool_local(0, 0x10000000) {}

void pool_local::init(unsigned object_size, unsigned stat_interval)
{
    assert(!all_slabs_cnt);
    timer = this->stat_interval = stat_interval;
    this->object_size = object_size;
}

pool_local::~pool_local()
{
    // Move one_hold_partial and trash objects to slabs
    // while slabs owned by this local pool. Delete all free slabs.
    maintain(0);

    auto list_to_global = [this](slab_header *slab) {
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

void * pool_local::alloc()
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
            all_slabs_cnt += create_slabs(object_size, this, &free_slabs);
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

void pool_local::free(void *obj)
{
    slab_header *slab = slab_of_obj(obj);
    if (slab->owner.load(std::memory_order_relaxed) == this) {
        if (one_hold_partial)
            return return_obj_to_slab(obj, slab);
        else
            return void(one_hold_partial = obj);
    }

    boost::detail::spinlock::scoped_lock lock(slab->owner_lock);
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
    return result;
}

} // namespace falloc
