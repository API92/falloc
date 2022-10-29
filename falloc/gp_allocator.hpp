#pragma once

#include <cstddef>
#include <limits>
#include <memory>

#include <falloc/impexp.hpp>
#include <falloc/pool.hpp>

namespace falloc {

static constexpr unsigned idx_frac_bits = 2;

[[gnu::always_inline]]
constexpr inline unsigned log2floor(unsigned x)
{
    // Expected to be a single `bsr` insruction for amd64.
    // std::bit_floor and std::countl_zero are not used because they test for 0.
    return __builtin_clz(x) ^ (std::numeric_limits<unsigned>::digits - 1);
}

[[gnu::always_inline]]
constexpr unsigned size_to_idx(unsigned size)
{
    return log2floor((size << 1) - 1);
}

// Must be used only in one thread.
class FALLOC_IMPEXP gp_allocator_local {
public:
    static constexpr unsigned MAX_POOLED_SIZE = 1984;
    static constexpr unsigned LARGE_ALIGNMENT = 64;

    gp_allocator_local(size_t stat_interval = 0x10000000) noexcept;
    ~gp_allocator_local() = default;

    void * allocate(size_t size, size_t alignment = 0) noexcept;
    [[gnu::always_inline]] inline
    void * allocate_inline(size_t size, size_t alignment = 0) noexcept;
    void * allocate_zeroed(size_t size, size_t alignment = 0) noexcept;
    void free(void *p) noexcept;
    [[gnu::always_inline]] inline void free_inline(void *p) noexcept;
    void * resize(void *p, size_t size) noexcept;
    size_t usable_size(void *p) noexcept;

private:
    struct large_obj_header;

    gp_allocator_local(gp_allocator_local const &) = delete;
    gp_allocator_local(gp_allocator_local &&) = delete;
    void operator = (gp_allocator_local const &) = delete;
    void operator = (gp_allocator_local &&) = delete;

    pool_local pools_[size_to_idx(MAX_POOLED_SIZE) + 1];
};

} // namespace falloc
