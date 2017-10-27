#include <falloc/object_pool.hpp>
#include <falloc/pool.hpp>

namespace falloc {

object_pool_impl::object_pool_impl(size_t object_size, unsigned stat_interval)
    : local(new pool_local(object_size, stat_interval))
{
}

object_pool_impl::~object_pool_impl() {}

void * object_pool_impl::alloc() noexcept
{
    return local->alloc();
}

void object_pool_impl::free(void *obj) noexcept
{
    local->free(obj);
}

bool object_pool_impl::maintain_local() noexcept
{
    return local->maintain(local->slabs_limit);
}

bool object_pool_impl::clear_local() noexcept
{
    return local->maintain(0);
}


} // namespace falloc
