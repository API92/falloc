#include <falloc/cache.hpp>

namespace falloc {

namespace detail {

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
