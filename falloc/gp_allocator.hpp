#pragma once

#include <cstddef>
#include <limits>
#include <memory>

#include <falloc/impexp.hpp>
#include <falloc/pool.hpp>

namespace falloc {

static constexpr unsigned idx_frac_bits = 2;

constexpr unsigned log2floor(unsigned x)
{
    return __builtin_clz(x)  ^ (std::numeric_limits<unsigned>::digits - 1);
}

constexpr unsigned size_to_idx(unsigned size)
{
    unsigned lg = log2floor(size);
    return (lg << idx_frac_bits) + ((size - 1) >> (lg - idx_frac_bits));
}

struct pool_local;

// Must be used only in one thread.
class FALLOC_IMPEXP gp_allocator_local {
public:
    static constexpr unsigned MAX_POOLED_SIZE = 2000;
    gp_allocator_local(size_t stat_interval = 0x10000000) noexcept;
    ~gp_allocator_local() = default;

    void * allocate(size_t size) noexcept;
    void free(void *p) noexcept;

private:
    gp_allocator_local(gp_allocator_local const &) = delete;
    gp_allocator_local(gp_allocator_local &&) = delete;
    void operator = (gp_allocator_local const &) = delete;
    void operator = (gp_allocator_local &&) = delete;

    pool_local pools_[size_to_idx(MAX_POOLED_SIZE) + 1];
};

FALLOC_IMPEXP void * allocate(size_t size) noexcept;
FALLOC_IMPEXP void free(void *ptr) noexcept;
FALLOC_IMPEXP bool maintain_allocator() noexcept;

} // namespace falloc
