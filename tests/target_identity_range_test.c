#include "target_identity.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static int test_anchor __attribute__((section(".data"))) = 0x41a71d3;

static int fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(void) {
    DavinciMappingIdentity identity;
    if (davinci_find_mapping_identity((uintptr_t)&test_anchor, &identity) != 0)
        return fail("locate executable data mapping");
    if (identity.start >= identity.end ||
        (uintptr_t)&test_anchor < identity.start ||
        (uintptr_t)&test_anchor >= identity.end)
        return fail("mapping identity contains queried address");
    if ((identity.protection & (PROT_READ | PROT_WRITE)) !=
        (PROT_READ | PROT_WRITE))
        return fail("executable data mapping protections are preserved");
    if (identity.path_length == 0U ||
        identity.path_length >= DAVINCI_MAPPING_PATH_MAX ||
        identity.path[identity.path_length] != 0U)
        return fail("mapped executable path is copied with a terminator");

    const long page_size_value = sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) return fail("query runtime page size");
    const size_t page_size = (size_t)page_size_value;
    uintptr_t relay_candidate = 0U;
    if (davinci_find_unmapped_range((uintptr_t)UINT64_C(0x40000000),
                                    (uintptr_t)UINT64_C(0x85e3deb5), page_size,
                                    page_size, &relay_candidate) != 0)
        return fail("find free page in target relay interval");
    if ((relay_candidate & (page_size - 1U)) != 0U ||
        relay_candidate < (uintptr_t)UINT64_C(0x40000000) ||
        relay_candidate > (uintptr_t)UINT64_C(0x85e3deb5) - page_size)
        return fail("relay candidate is aligned and wholly inside interval");

    DavinciMappingIdentity absent_identity;
    if (davinci_find_mapping_identity(relay_candidate, &absent_identity) == 0)
        return fail("candidate address is not already mapped");

    uintptr_t invalid_candidate = 0U;
    if (davinci_find_unmapped_range((uintptr_t)UINT64_C(0x40000000),
                                    (uintptr_t)UINT64_C(0x85e3deb5), page_size,
                                    page_size + 1U, &invalid_candidate) == 0)
        return fail("non-power-of-two alignment is rejected");
    if (davinci_find_unmapped_range(UINTPTR_MAX - 8U, UINTPTR_MAX,
                                    16U, page_size, &invalid_candidate) == 0)
        return fail("overflowing requested range is rejected");
    if (davinci_find_mapping_identity(0U, NULL) == 0)
        return fail("null mapping identity output is rejected");

    printf("PASS: mapping identity start=%#lx end=%#lx prot=%#x path=%.*s\n",
           (unsigned long)identity.start, (unsigned long)identity.end,
           identity.protection, (int)identity.path_length, identity.path);
    printf("PASS: read-only relay candidate=%#lx page_size=%zu\n",
           (unsigned long)relay_candidate, page_size);
    puts("target identity mapping/range checks: 6/6 passed");
    return 0;
}
