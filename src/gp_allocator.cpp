#include <falloc/gp_allocator.hpp>

#include <atomic>
#include <limits>
#include <algorithm>

#include <falloc/pool.hpp>

#include <sys/mman.h>

namespace falloc {


namespace
{

//
// global_trash
//
//

class global_trash {
public:
    struct already_initialized {};

    global_trash() noexcept;
    global_trash(already_initialized) noexcept;

    inline void add_region(void *p, size_t size) noexcept;
    inline bool clean() noexcept;

private:
    std::atomic<void *> free_list_;
};


[[maybe_unused]]
global_trash::global_trash() noexcept : free_list_(nullptr) {}

global_trash::global_trash(already_initialized) noexcept {}

inline void global_trash::add_region(void *p, size_t size) noexcept
{
    reinterpret_cast<size_t *>(p)[1] = size;
    *reinterpret_cast<void **>(p) = free_list_.load(std::memory_order_relaxed);
    while (!free_list_.compare_exchange_weak(
                *reinterpret_cast<void **>(p), p,
                std::memory_order_release, std::memory_order_relaxed)) {}
}

inline bool global_trash::clean() noexcept
{
    if (!free_list_.load(std::memory_order_relaxed))
        return false;

    bool result = false;
    for (bool unmaped = true; unmaped;) {
        unmaped = false;
        void *p = free_list_.exchange(nullptr, std::memory_order_consume);
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

} // namespace


//
// gp_allocator_local
//

gp_allocator_local::gp_allocator_local(size_t stat_interval) noexcept
{
    for (size_t sz = 0; sz <= MAX_POOLED_SIZE; ++sz) {
        size_t idx = size_to_idx(sz);
        if (pools_[idx].object_size)
            continue;
        pools_[idx].init(std::max(sz, sizeof(void *)), stat_interval);
    }
}

void * gp_allocator_local::allocate(size_t size) noexcept
{
    if (size <= MAX_POOLED_SIZE)
        return pools_[size_to_idx(size)].alloc();
    else {
        size_t pages_cnt =
            (size + 2 * sizeof(unsigned) + PAGESIZE - 1) / PAGESIZE;
        if (pages_cnt > std::numeric_limits<unsigned>::max())
            return nullptr;

        unsigned *p = (unsigned *)mmap(nullptr, pages_cnt * PAGESIZE,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            return nullptr;
        p[0] = MAX_POOLED_SIZE + 1;
        p[1] = static_cast<unsigned>(pages_cnt);
        return p + 64 / sizeof(unsigned);

        return nullptr;
    }
}

void gp_allocator_local::free(void *p) noexcept
{
    unsigned *size = reinterpret_cast<unsigned *>(slab_of_obj(p));
    if (*size <= MAX_POOLED_SIZE) {
        pools_[size_to_idx(*size)].free(p);
        global_trash.clean();
        return;
    }
    else {
        unsigned pages_cnt = size[1];
        if (munmap(p, pages_cnt * PAGESIZE) != 0)
            global_trash.add_region(p, pages_cnt * PAGESIZE);
        global_trash.clean();
    }
}

static thread_local gp_allocator_local default_gp_allocator;

void * allocate(size_t size) noexcept
{
    return default_gp_allocator.allocate(size);
}

void free(void *p) noexcept
{
    return default_gp_allocator.free(p);
}

bool maintain_allocator() noexcept
{
    return global_trash.clean() | maintain_all_pools();
}

} // namespace falloc
