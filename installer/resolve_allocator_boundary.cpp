#include "resolve_allocator_boundary.h"

#include <new>

extern "C" void *davinci_av1_allocate_resolve_node_noexcept(
    uintptr_t allocator_target, size_t size) {
    if (allocator_target == 0U || size != 0x28U) return nullptr;
    using AllocateFunction = void *(*)(size_t);
    auto allocate = reinterpret_cast<AllocateFunction>(allocator_target);
    try {
        return allocate(size);
    } catch (const std::bad_alloc &) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" int davinci_av1_free_resolve_node_noexcept(
    uintptr_t sized_delete_target, void *node, size_t size) {
    if (sized_delete_target == 0U || node == nullptr || size != 0x28U)
        return -1;
    using DeleteFunction = void (*)(void *, size_t);
    auto release = reinterpret_cast<DeleteFunction>(sized_delete_target);
    try {
        release(node, size);
    } catch (...) {
        return -1;
    }
    return 0;
}
