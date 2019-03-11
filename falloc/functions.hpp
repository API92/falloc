#pragma once

#include "impexp.hpp"

#include <cstddef>

namespace falloc
{

// Allocate memory from default general purpose allocator.
FALLOC_IMPEXP void * allocate(size_t size) noexcept;
// Free memory allocated by allocate function above.
FALLOC_IMPEXP void free(void *ptr) noexcept;

/// Returns trash in pools into slabs.
FALLOC_IMPEXP bool maintain_all_pools() noexcept;
/// Returns trash in pools into slabs and delete all free slabs.
FALLOC_IMPEXP bool clear_all_pools() noexcept;
FALLOC_IMPEXP void new_handler();


} // namespace falloc
