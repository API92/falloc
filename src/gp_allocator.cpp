#include <falloc/gp_allocator.hpp>

#include <limits>
#include <algorithm>

#include <falloc/pool.hpp>

#include <sys/mman.h>

namespace falloc {

gp_allocator::gp_allocator(size_t stat_interval) noexcept
{
    for (size_t sz = 0; sz <= MAX_POOLED_SIZE; ++sz) {
        size_t idx = size_to_idx(sz);
        if (_u.pools[idx].object_size)
            continue;
        _u.pools[idx].init(std::max(sz, sizeof(void *)), stat_interval);
    }
}

void * gp_allocator::allocate(size_t size) noexcept
{
    if (size <= MAX_POOLED_SIZE)
        return _u.pools[size_to_idx(size)].alloc();
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
        return p + 2;

        return nullptr;
    }
}

void gp_allocator::free(void *p) noexcept
{
    unsigned *size = reinterpret_cast<unsigned *>(slab_of_obj(p));
    if (*size <= MAX_POOLED_SIZE)
        _u.pools[size_to_idx(*size)].free(p);
    else {
        unsigned pages_cnt = size[1];
        if (munmap(size, pages_cnt * PAGESIZE) == 0)
            return;
        else
            std::abort();
    }
}

static thread_local gp_allocator default_gp_allocator;

void * allocate(size_t size) noexcept
{
    return default_gp_allocator.allocate(size);
}

void free(void *p) noexcept
{
    return default_gp_allocator.free(p);
}

} // namespace falloc
