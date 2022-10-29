#pragma once

#include "impexp.hpp"

#include <cstddef>

namespace falloc
{

// Allocate memory from default general purpose allocator.
FALLOC_IMPEXP void * malloc(size_t size) noexcept;
// Allocate zeroed memory from default general purpose allocator.
FALLOC_IMPEXP void * calloc(size_t num, size_t size) noexcept;
// Implementation of a standard realloc function.
FALLOC_IMPEXP void * realloc(void *p, size_t size) noexcept;
// Implementation of a standard aligned_alloc function.
FALLOC_IMPEXP void * aligned_alloc(size_t alignment, size_t size) noexcept;
// Free memory allocated by allocate function above.
FALLOC_IMPEXP void free(void *ptr) noexcept;
// Returns the number of usable bytes in the block pointed by ptr.
FALLOC_IMPEXP size_t usable_size(void *ptr);

/// Returns trash in pools into slabs.
FALLOC_IMPEXP bool maintain_all_pools() noexcept;
/// Returns trash in pools into slabs and delete all free slabs.
FALLOC_IMPEXP bool clear_all_pools() noexcept;
FALLOC_IMPEXP void new_handler();


} // namespace falloc
