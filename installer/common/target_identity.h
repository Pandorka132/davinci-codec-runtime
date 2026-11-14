#ifndef DAVINCI_TARGET_IDENTITY_H
#define DAVINCI_TARGET_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

int davinci_compute_process_file_identity(uint8_t executable_sha256[32],
                                          uint8_t loaded_dso_set_sha256[32],
                                          uint32_t *loaded_dso_count);
int davinci_read_self_start_ticks(uint64_t *result);
int davinci_count_self_threads(uint32_t *result);
int davinci_find_mapping_protection(uintptr_t address, int *protection);
int davinci_find_mapping_range_protection(uintptr_t address, size_t length,
                                          int *protection);

#define DAVINCI_MAPPING_PATH_MAX 4096U

typedef struct {
    uintptr_t start;
    uintptr_t end;
    uint32_t protection;
    uint32_t device_major;
    uint32_t device_minor;
    uint64_t inode;
    size_t path_length;
    uint8_t path[DAVINCI_MAPPING_PATH_MAX];
} DavinciMappingIdentity;

/* Exact /proc/self/maps record containing address; fails closed if any line
 * cannot be parsed or the requested address is not currently mapped. */
int davinci_find_mapping_identity(uintptr_t address,
                                  DavinciMappingIdentity *identity);

/* Find a page-aligned hole wholly inside [range_start, range_end). This is a
 * read-only observation; callers must still use MAP_FIXED_NOREPLACE later. */
int davinci_find_unmapped_range(uintptr_t range_start, uintptr_t range_end,
                                size_t length, size_t alignment,
                                uintptr_t *candidate);

#ifdef DAVINCI_TARGET_IDENTITY_TEST
int davinci_test_parse_maps_line(const uint8_t *line, size_t length,
                                 uint64_t *start, uint64_t *end,
                                 uint32_t *device_major,
                                 uint32_t *device_minor, uint64_t *inode,
                                 uint8_t *path, size_t path_capacity,
                                 size_t *path_length);
int davinci_test_parse_mountinfo_line(const uint8_t *line, size_t length,
                                      uint64_t *mount_id,
                                      uint32_t *device_major,
                                      uint32_t *device_minor);
int davinci_test_lookup_mount_device_text(const uint8_t *text, size_t length,
                                          uint64_t mount_id,
                                          uint32_t *device_major,
                                          uint32_t *device_minor);
#endif

#endif
