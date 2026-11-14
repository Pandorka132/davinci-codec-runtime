#ifndef DAVINCI_AV1_RESOLVE_ALLOCATOR_BOUNDARY_H
#define DAVINCI_AV1_RESOLVE_ALLOCATOR_BOUNDARY_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define DAVINCI_AV1_ALLOCATOR_BOUNDARY_HIDDEN \
    __attribute__((visibility("hidden")))
#else
#define DAVINCI_AV1_ALLOCATOR_BOUNDARY_HIDDEN
#endif

#ifdef __cplusplus
extern "C" {
#endif

DAVINCI_AV1_ALLOCATOR_BOUNDARY_HIDDEN
void *davinci_av1_allocate_resolve_node_noexcept(uintptr_t allocator_target,
                                                size_t size);
DAVINCI_AV1_ALLOCATOR_BOUNDARY_HIDDEN
int davinci_av1_free_resolve_node_noexcept(uintptr_t sized_delete_target,
                                           void *node, size_t size);

#ifdef __cplusplus
}
#endif

#undef DAVINCI_AV1_ALLOCATOR_BOUNDARY_HIDDEN

#endif
