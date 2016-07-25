/*
 * Copyright (C) Andrey Pikas
 */

#include <falloc/cache.hpp>

#include <atomic> // atomic
#include <bitset>
#include <cassert> // assert
#include <cstddef> // ptrdiff_t, offsetof
#include <cstdint> // intptr_t
#include <cstdlib> // abort
#include <memory> // align
#include <new> // new_handler, set_new_handler, get_new_handler

#include <boost/smart_ptr/detail/spinlock.hpp>
#include <sys/mman.h>

namespace falloc {
namespace detail {

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

///
/// slab_header
///

namespace {

const long PAGESIZE = sysconf(_SC_PAGE_SIZE);
const uintptr_t HEADER_MASK = ~(uintptr_t)PAGESIZE + 1;

} // namespace

struct slab_header {
    void *free = nullptr;
    slab_header *next = nullptr;
    slab_header *prev = nullptr;
    int used_cnt = 0;
    boost::detail::spinlock owner_lock;
    std::atomic<void *> owner = {nullptr};

    void init(unsigned align, unsigned size);
    inline void * alloc();
    inline void put_free(void *obj);
    inline static slab_header * slab_of_obj(void *obj);
};

void slab_header::init(unsigned align, unsigned size)
{
    // object must be properly aligned for pointer.
    if (align < alignof(void *))
        align = alignof(void *);
    // free object will contain pointer to the next free at its begining.
    if (size < sizeof(void *))
        size = sizeof(void *);
    assert(std::bitset<64>(align).count() == 1);
    unsigned step = (size + align - 1) & ~(align - 1);

    char *p = reinterpret_cast<char *>(this) + sizeof(*this);
    char *end = reinterpret_cast<char *>(this) + PAGESIZE;
    size_t space = end - p;
    if (!std::align(align, size, reinterpret_cast<void *&>(p), space))
        return;

    char *next = p + step;
    do {
        *reinterpret_cast<void **>(p) = free;
        free = p;
        p = next;
        next += step;
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

inline slab_header * slab_header::slab_of_obj(void *obj)
{
    return reinterpret_cast<slab_header *>((uintptr_t)obj & HEADER_MASK);
}

int create_slabs(size_t align, size_t object_size, void *owner,
        slab_header **list)
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
        slab->init(align, object_size);
        slab->owner.store(owner, std::memory_order_relaxed);
        slab->next = *list;
        *list = slab;
    }
    return cnt;
}

void delete_slab(slab_header *slab)
{
    // At this moment slab can't be used from other threads.
    if (!munmap(slab, PAGESIZE))
        return;
    std::abort();
}


///
/// cache_global
///

struct cache_global : list_node<cache_global> {
    std::atomic<void *> trash = {nullptr};
    std::atomic_flag clean_lock = ATOMIC_FLAG_INIT;

    cache_global();
    ~cache_global();
    bool maintain();
};

namespace {

list_node<cache_global> global_maintain_list;
boost::detail::spinlock global_maintain_list_lock = BOOST_DETAIL_SPINLOCK_INIT;

} // namespace

cache_global::cache_global()
{
    boost::detail::spinlock::scoped_lock lock(global_maintain_list_lock);
    global_maintain_list.append(this);
}

cache_global::~cache_global()
{
    global_maintain_list_lock.lock();
    remove(); // remove from global_maintain_list
    global_maintain_list_lock.unlock();

    maintain();
}

bool cache_global::maintain()
{
    if (!trash.load(std::memory_order_relaxed))
        return false;
    if (clean_lock.test_and_set(std::memory_order_acquire))
        return false;
    bool result = false;
    void *trash_mtn = trash.exchange(nullptr, std::memory_order_acquire);
    while (trash_mtn) {
        void *trash_del = trash_mtn;
        trash_mtn = *reinterpret_cast<void **>(trash_mtn);
        slab_header *slab = slab_header::slab_of_obj(trash_del);
        slab->put_free(trash_del);
        if (!slab->used_cnt) {
            delete_slab(slab);
            result = true;
        }
    }
    // Unlock only after loop to protect slabs of objects from trash,
    // not only trash pointer itself. This slabs are owned by cache_global
    // (but not pointed explicitly by it).
    clean_lock.clear(std::memory_order_release);
    return result;
}

cache_global * make_cache_global()
{
    return new cache_global;
}

void free_cache_global(cache_global *p)
{
    delete p;
}


///
/// cache_local
///

// Used in single thread (except trash field).
struct cache_local : list_node<cache_local> {
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

    size_t align; // alignment of cached objects
    size_t object_size; // size of cached objects
    size_t slabs_limit = ~0ULL; // maximum number of slabs (soft limit)

    // list for objects, freed from foreign threads
    std::atomic<void *> trash = {nullptr};

    cache_local(unsigned stat_interval, size_t align, size_t object_size);
    ~cache_local();

    // push slab into the begining of the list
    inline static void push(slab_header **list, slab_header *slab);
    // remove slab from double linked list
    inline static void remove_from_list(slab_header **list, slab_header *slab);
    // put obj to slab's free list and move slab between full/partial/free lists
    inline void return_obj_to_slab(void *obj, slab_header *slab);
    // allocate one object.
    inline void * alloc();
    // free object obj, which belongs to slab.
    inline void free(void *obj, slab_header *slab);
    // return objects from trash to their slabs, leave maximum free_slabs_limit
    // slabs in free list.
    bool maintain(size_t slabs_limit);
};

namespace {

thread_local list_node<cache_local> local_maintain_list;

} // namespace

cache_local::cache_local(unsigned stat_interval, size_t align,
        size_t object_size)
    : timer(stat_interval), stat_interval(stat_interval), align(align),
    object_size(object_size)
{
    local_maintain_list.append(this);
}

cache_local::~cache_local()
{
    remove(); // remove from local_maintain_list
}

inline void cache_local::push(slab_header **list, slab_header *slab)
{
    slab->next = *list;
    slab->prev = nullptr;
    if (slab->next)
        slab->next->prev = slab;
    *list = slab;
}

inline void cache_local::remove_from_list(slab_header **list, slab_header *slab)
{
    if (slab->next)
        slab->next->prev = slab->prev;
    if (slab->prev)
        slab->prev->next = slab->next;
    if (*list == slab)
        *list = slab->next;
}

inline void cache_local::return_obj_to_slab(void *obj, slab_header *slab)
{
    if (!slab->free) {
        // slab in full list now. Move it to partial.
        remove_from_list(&full_slabs, slab);
        push(&partial_slabs, slab);
    }
    else if (slab->used_cnt == 1) {
        // slab will be free after putting object. Move it from partial to free.
        remove_from_list(&partial_slabs, slab);
        slab->next = free_slabs;
        free_slabs = slab;
        --used_slabs_cnt;
    }
    slab->put_free(obj);
}

inline void * cache_local::alloc()
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
            all_slabs_cnt +=
                create_slabs(align, object_size, this, &free_slabs);
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

inline void cache_local::free(void *obj, slab_header *slab)
{
    if (one_hold_partial)
        return_obj_to_slab(obj, slab);
    else
        one_hold_partial = obj;
}

bool cache_local::maintain(size_t limit)
{
    bool result = false;
    if (one_hold_partial) {
        return_obj_to_slab(one_hold_partial,
                slab_header::slab_of_obj(one_hold_partial));
        one_hold_partial = nullptr;
        result = true;
    }

    if (trash.load(std::memory_order_relaxed)) {
        void *trash_mtn = trash.exchange(nullptr, std::memory_order_acquire);
        while (trash_mtn) {
            void *trash_rec = trash_mtn;
            trash_mtn = *reinterpret_cast<void **>(trash_mtn);
            // Objects in trash lays in slabs of this cache_local.
            return_obj_to_slab(trash_rec, slab_header::slab_of_obj(trash_rec));
            result = true;
        }
    }

    while (all_slabs_cnt > limit && free_slabs) {
        slab_header *p = free_slabs;
        free_slabs = free_slabs->next;
        --all_slabs_cnt;
        delete_slab(p);
        result = true;
    }

    if (!timer) {
        slabs_limit = stat_max_used_cnt;
        stat_max_used_cnt = used_slabs_cnt;
        timer = stat_interval;
    }
    return result;
}


///
/// cache
///

cache::cache(cache_global *global, unsigned stat_interval, size_t align,
        size_t object_size)
    : global(global), local(new cache_local(stat_interval, align, object_size))
{
}

cache::~cache()
{
    // Move one_hold_partial and trash objects to slabs
    // while slabs owned by this local cache. Delete all free slabs.
    local->maintain(0);

    auto list_to_global = [this](slab_header *slab) {
        while (slab) {
            // After unlock slab may be deleted. So use it before.
            slab_header *next = slab->next;
            slab->owner_lock.lock();
            slab->owner.store(global, std::memory_order_relaxed);
            slab->owner_lock.unlock();
            slab = next;
        }
    };
    list_to_global(local->partial_slabs);
    list_to_global(local->full_slabs);

    // All slabs now owned by global cache.
    // Local trash is unchangeable after moving all slabs to global.
    void *first_trash = local->trash.load(std::memory_order_acquire);
    if (first_trash) {
        void *last_trash = first_trash;
        while (*reinterpret_cast<void **>(last_trash))
            last_trash = *reinterpret_cast<void **>(last_trash);
        // Note: the below use is not thread-safe in at least
        // GCC prior to 4.8.3 (bug 60272), clang prior to 2014-05-05 (bug 18899)
        // MSVC prior to 2014-03-17 (bug 819819). So update your compiler.
        *reinterpret_cast<void **>(last_trash) =
            global->trash.load(std::memory_order_relaxed);
        while (!global->trash.compare_exchange_weak(
                    *reinterpret_cast<void **>(last_trash), first_trash,
                    std::memory_order_release, std::memory_order_relaxed)) {}
    }
}

void * cache::alloc() noexcept
{
    return local->alloc();
}

void cache::free(void *obj) noexcept
{
    slab_header *slab = slab_header::slab_of_obj(obj);
    if (slab->owner.load(std::memory_order_relaxed) == local.get()) {
        local->free(obj, slab);
        return;
    }

    boost::detail::spinlock::scoped_lock lock(slab->owner_lock);
    void *owner = slab->owner.load(std::memory_order_relaxed);
    std::atomic<void *> &trash = (owner == global
        ? global->trash
        : reinterpret_cast<cache_local *>(owner)->trash);
    // Note: the below use is not thread-safe in at least
    // GCC prior to 4.8.3 (bug 60272), clang prior to 2014-05-05 (bug 18899)
    // MSVC prior to 2014-03-17 (bug 819819). So update your compiler.
    *reinterpret_cast<void **>(obj) = trash.load(std::memory_order_relaxed);
    while (!trash.compare_exchange_weak(*reinterpret_cast<void **>(obj), obj,
                std::memory_order_release, std::memory_order_relaxed)) {}
}

bool cache::maintain_global() noexcept
{
    return global->maintain();
}

bool cache::maintain_local() noexcept
{
    return local->maintain(local->slabs_limit);
}

bool cache::clear_local() noexcept
{
    return local->maintain(0);
}

void * cache::alloc_with_new_handler() noexcept
{
    if (void *result = alloc())
        return result;
    while (std::new_handler handler = std::get_new_handler())
        try {
            handler();
            if (void *result = alloc())
                return result;
        }
        catch (...) {
            return nullptr;
        }
    return nullptr;
}

} // namespace detail

bool maintain_all_caches() noexcept
{
    bool result = false;
    for (detail::list_node<detail::cache_local> *e =
            &detail::local_maintain_list, *l = e->next; l != e; l = l->next) {
        detail::cache_local *c = l->get();
        result |= c->maintain(c->slabs_limit);
    }

    if (!detail::global_maintain_list_lock.try_lock())
        return result;
    for (detail::list_node<detail::cache_global> *e =
            &detail::global_maintain_list, *l = e->next; l != e; l = l->next)
        result |= l->get()->maintain();
    detail::global_maintain_list_lock.unlock();
    return result;
}

bool clear_all_caches() noexcept
{
    bool result = false;
    for (detail::list_node<detail::cache_local> *e =
            &detail::local_maintain_list, *l = e->next; l != e; l = l->next) {
        detail::cache_local *c = l->get();
        result |= c->maintain(0);
    }

    if (!detail::global_maintain_list_lock.try_lock())
        return result;
    for (detail::list_node<detail::cache_global> *e =
            &detail::global_maintain_list, *l = e->next; l != e; l = l->next)
        result |= l->get()->maintain();
    detail::global_maintain_list_lock.unlock();
    return result;
}

namespace {

std::new_handler old_new_handler = nullptr;

struct push_new_handler {
    push_new_handler()
    {
        old_new_handler = std::set_new_handler(new_handler);
    }

    ~push_new_handler()
    {
        std::set_new_handler(old_new_handler);
    }
} push_new_handler_;

} // namespace

void new_handler()
{
    if (maintain_all_caches())
        return;
    if (clear_all_caches())
        return;
    if (old_new_handler) {
        old_new_handler();
        return;
    }
    throw std::bad_alloc();
}

} // namespace falloc
