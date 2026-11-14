#define _GNU_SOURCE
#include "target_identity.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/stat.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef SYS_getdents64
#error "SYS_getdents64 is required for allocation-free thread enumeration"
#endif

#ifndef SYS_statx
#error "SYS_statx is required for mapped-file mount identity validation"
#endif

#ifndef STATX_MNT_ID
#error "STATX_MNT_ID is required for mapped-file mount identity validation"
#endif

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

enum {
    MAX_EXECUTABLE_DSOS = 256,
    MAPS_LINE_MAX = 8192,
    MAPS_READ_BUFFER_SIZE = 16384,
    FILE_HASH_BUFFER_SIZE = 64 * 1024,
    TASK_READ_BUFFER_SIZE = 4096
};

typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t offset;
    uint64_t inode;
    uint32_t device_major;
    uint32_t device_minor;
    char permissions[5];
    uint8_t path[PATH_MAX];
    size_t path_length;
} ParsedMapsLine;

typedef struct {
    int descriptor;
    uint8_t read_buffer[MAPS_READ_BUFFER_SIZE];
    size_t read_offset;
    size_t read_length;
    uint8_t line[MAPS_LINE_MAX];
    size_t line_length;
} ProcTextReader;

typedef struct {
    uint8_t path[PATH_MAX];
    size_t path_length;
    uint32_t device_major;
    uint32_t device_minor;
    uint64_t inode;
    uint8_t sha256[32];
    int descriptor;
    struct stat final_status;
} MappedFileIdentity;

struct linux_dirent64_local {
    uint64_t inode;
    int64_t offset;
    unsigned short record_length;
    unsigned char type;
    char name[];
};

static bool file_stat_unchanged(const struct stat *before,
                                const struct stat *after) {
    return before->st_dev == after->st_dev &&
           before->st_ino == after->st_ino &&
           before->st_size == after->st_size &&
           before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
           before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
           before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
           before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

static int parse_unsigned(const uint8_t *data, size_t length, size_t *offset,
                          unsigned base, uint64_t *value) {
    if (data == NULL || offset == NULL || value == NULL ||
        (base != 10U && base != 16U))
        return -1;

    size_t cursor = *offset;
    uint64_t parsed = 0;
    size_t digits = 0;
    while (cursor < length) {
        uint8_t character = data[cursor];
        unsigned digit;
        if (character >= '0' && character <= '9')
            digit = (unsigned)(character - '0');
        else if (base == 16U && character >= 'a' && character <= 'f')
            digit = 10U + (unsigned)(character - 'a');
        else if (base == 16U && character >= 'A' && character <= 'F')
            digit = 10U + (unsigned)(character - 'A');
        else
            break;

        if (digit >= base || parsed > (UINT64_MAX - digit) / base)
            return -1;
        parsed = parsed * base + digit;
        ++cursor;
        ++digits;
    }
    if (digits == 0U) return -1;
    *offset = cursor;
    *value = parsed;
    return 0;
}

static bool is_map_separator(uint8_t character) {
    return character == ' ' || character == '\t';
}

static int skip_map_separators(const uint8_t *line, size_t length,
                               size_t *offset) {
    size_t cursor = *offset;
    size_t initial = cursor;
    while (cursor < length && is_map_separator(line[cursor])) ++cursor;
    if (cursor == initial) return -1;
    *offset = cursor;
    return 0;
}

static int parse_maps_line(const uint8_t *line, size_t length,
                           ParsedMapsLine *parsed) {
    if (line == NULL || parsed == NULL || length == 0U ||
        length > MAPS_LINE_MAX)
        return -1;

    memset(parsed, 0, sizeof(*parsed));
    size_t cursor = 0;
    if (parse_unsigned(line, length, &cursor, 16U, &parsed->start) != 0 ||
        cursor >= length || line[cursor++] != '-' ||
        parse_unsigned(line, length, &cursor, 16U, &parsed->end) != 0 ||
        parsed->end <= parsed->start ||
        skip_map_separators(line, length, &cursor) != 0)
        return -1;

    if (length - cursor < 4U) return -1;
    parsed->permissions[0] = (char)line[cursor++];
    parsed->permissions[1] = (char)line[cursor++];
    parsed->permissions[2] = (char)line[cursor++];
    parsed->permissions[3] = (char)line[cursor++];
    parsed->permissions[4] = '\0';
    if ((parsed->permissions[0] != 'r' && parsed->permissions[0] != '-') ||
        (parsed->permissions[1] != 'w' && parsed->permissions[1] != '-') ||
        (parsed->permissions[2] != 'x' && parsed->permissions[2] != '-') ||
        (parsed->permissions[3] != 'p' && parsed->permissions[3] != 's') ||
        skip_map_separators(line, length, &cursor) != 0)
        return -1;

    if (parse_unsigned(line, length, &cursor, 16U, &parsed->offset) != 0 ||
        skip_map_separators(line, length, &cursor) != 0)
        return -1;

    uint64_t device_major = 0;
    uint64_t device_minor = 0;
    if (parse_unsigned(line, length, &cursor, 16U, &device_major) != 0 ||
        cursor >= length || line[cursor++] != ':' ||
        parse_unsigned(line, length, &cursor, 16U, &device_minor) != 0 ||
        device_major > UINT32_MAX || device_minor > UINT32_MAX ||
        skip_map_separators(line, length, &cursor) != 0 ||
        parse_unsigned(line, length, &cursor, 10U, &parsed->inode) != 0)
        return -1;
    parsed->device_major = (uint32_t)device_major;
    parsed->device_minor = (uint32_t)device_minor;

    while (cursor < length && is_map_separator(line[cursor])) ++cursor;
    parsed->path_length = length - cursor;
    if (parsed->path_length >= sizeof(parsed->path)) return -1;
    for (size_t index = cursor; index < length; ++index) {
        if (line[index] == 0U) return -1;
    }
    if (parsed->path_length != 0U)
        memcpy(parsed->path, line + cursor, parsed->path_length);
    return 0;
}

static int parse_mountinfo_line(const uint8_t *line, size_t length,
                                uint64_t *mount_id,
                                uint32_t *device_major,
                                uint32_t *device_minor) {
    if (line == NULL || mount_id == NULL || device_major == NULL ||
        device_minor == NULL || length == 0U || length > MAPS_LINE_MAX)
        return -1;

    size_t cursor = 0U;
    uint64_t parsed_mount_id = 0U;
    uint64_t parent_id = 0U;
    uint64_t parsed_major = 0U;
    uint64_t parsed_minor = 0U;

    if (parse_unsigned(line, length, &cursor, 10U, &parsed_mount_id) != 0 ||
        parsed_mount_id == 0U ||
        skip_map_separators(line, length, &cursor) != 0 ||
        parse_unsigned(line, length, &cursor, 10U, &parent_id) != 0 ||
        skip_map_separators(line, length, &cursor) != 0 ||
        parse_unsigned(line, length, &cursor, 10U, &parsed_major) != 0 ||
        cursor >= length || line[cursor++] != ':' ||
        parse_unsigned(line, length, &cursor, 10U, &parsed_minor) != 0 ||
        parsed_major > UINT32_MAX || parsed_minor > UINT32_MAX ||
        skip_map_separators(line, length, &cursor) != 0)
        return -1;

    /* Require root, mount point and mount options fields after major:minor. */
    for (unsigned field = 0U; field < 3U; ++field) {
        size_t token_start = cursor;
        while (cursor < length && !is_map_separator(line[cursor]))
            ++cursor;
        if (cursor == token_start) return -1;
        if (field != 2U &&
            skip_map_separators(line, length, &cursor) != 0)
            return -1;
    }

    (void)parent_id;
    *mount_id = parsed_mount_id;
    *device_major = (uint32_t)parsed_major;
    *device_minor = (uint32_t)parsed_minor;
    return 0;
}

#ifdef DAVINCI_TARGET_IDENTITY_TEST
static int lookup_mount_device_text(const uint8_t *text, size_t length,
                                    uint64_t wanted_mount_id,
                                    uint32_t *device_major,
                                    uint32_t *device_minor) {
    if (text == NULL || length == 0U || wanted_mount_id == 0U ||
        device_major == NULL || device_minor == NULL)
        return -1;

    size_t offset = 0U;
    unsigned matches = 0U;
    uint32_t found_major = 0U;
    uint32_t found_minor = 0U;

    while (offset < length) {
        size_t line_start = offset;
        while (offset < length && text[offset] != '\n') ++offset;
        if (offset == length) return -1;

        size_t line_length = offset - line_start;
        ++offset;
        uint64_t current_mount_id = 0U;
        uint32_t current_major = 0U;
        uint32_t current_minor = 0U;
        if (parse_mountinfo_line(text + line_start, line_length,
                                 &current_mount_id, &current_major,
                                 &current_minor) != 0)
            return -1;
        if (current_mount_id == wanted_mount_id) {
            found_major = current_major;
            found_minor = current_minor;
            ++matches;
        }
    }

    if (matches != 1U) return -1;
    *device_major = found_major;
    *device_minor = found_minor;
    return 0;
}
#endif

static int proc_text_reader_init(ProcTextReader *reader,
                                 const char *path) {
    if (reader == NULL || path == NULL) return -1;
    memset(reader, 0, sizeof(*reader));
    reader->descriptor = open(path, O_RDONLY | O_CLOEXEC);
    return reader->descriptor >= 0 ? 0 : -1;
}

/* Returns 1 for a line, 0 for EOF, and -1 for I/O or line overflow. */
static int proc_text_reader_next(ProcTextReader *reader, const uint8_t **line,
                            size_t *line_length) {
    if (reader == NULL || line == NULL || line_length == NULL ||
        reader->descriptor < 0)
        return -1;

    reader->line_length = 0;
    for (;;) {
        if (reader->read_offset == reader->read_length) {
            ssize_t amount;
            do {
                amount = read(reader->descriptor, reader->read_buffer,
                              sizeof(reader->read_buffer));
            } while (amount < 0 && errno == EINTR);
            if (amount < 0) return -1;
            if (amount == 0) {
                return reader->line_length == 0U ? 0 : -1;
            }
            reader->read_offset = 0;
            reader->read_length = (size_t)amount;
        }

        uint8_t character = reader->read_buffer[reader->read_offset++];
        if (character == '\n') {
            *line = reader->line;
            *line_length = reader->line_length;
            return reader->line_length == 0U ? -1 : 1;
        }
        if (reader->line_length >= sizeof(reader->line)) return -1;
        reader->line[reader->line_length++] = character;
    }
}

static int proc_text_reader_close(ProcTextReader *reader) {
    if (reader == NULL || reader->descriptor < 0) return -1;
    int descriptor = reader->descriptor;
    reader->descriptor = -1;
    return close(descriptor);
}

static int lookup_mount_device(uint64_t wanted_mount_id,
                                uint32_t *device_major,
                                uint32_t *device_minor) {
    if (wanted_mount_id == 0U || device_major == NULL ||
        device_minor == NULL)
        return -1;

    ProcTextReader reader;
    if (proc_text_reader_init(&reader, "/proc/self/mountinfo") != 0)
        return -1;

    unsigned matches = 0U;
    uint32_t found_major = 0U;
    uint32_t found_minor = 0U;
    bool failed = false;

    for (;;) {
        const uint8_t *line = NULL;
        size_t line_length = 0U;
        int line_result = proc_text_reader_next(&reader, &line, &line_length);
        if (line_result == 0) break;
        if (line_result < 0) {
            failed = true;
            break;
        }

        uint64_t current_mount_id = 0U;
        uint32_t current_major = 0U;
        uint32_t current_minor = 0U;
        if (parse_mountinfo_line(line, line_length, &current_mount_id,
                                 &current_major, &current_minor) != 0) {
            failed = true;
            break;
        }
        if (current_mount_id == wanted_mount_id) {
            found_major = current_major;
            found_minor = current_minor;
            ++matches;
            if (matches > 1U) {
                failed = true;
                break;
            }
        }
    }

    if (proc_text_reader_close(&reader) != 0) failed = true;
    if (failed || matches != 1U) return -1;

    *device_major = found_major;
    *device_minor = found_minor;
    return 0;
}

static int descriptor_matches_mapped_device(int descriptor,
                                            uint32_t expected_major,
                                            uint32_t expected_minor) {
    if (descriptor < 0) return -1;

    struct statx status;
    memset(&status, 0, sizeof(status));
    long result = syscall(SYS_statx, descriptor, "", AT_EMPTY_PATH,
                          STATX_MNT_ID, &status);
    if (result != 0 || (status.stx_mask & STATX_MNT_ID) == 0U)
        return -1;

    uint32_t mount_major = 0U;
    uint32_t mount_minor = 0U;
    if (lookup_mount_device(status.stx_mnt_id, &mount_major,
                            &mount_minor) != 0)
        return -1;

    return mount_major == expected_major &&
           mount_minor == expected_minor ? 0 : -1;
}

static int hash_open_file(int descriptor, uint8_t digest[32],
                          struct stat *final_status) {
    if (descriptor < 0 || digest == NULL || final_status == NULL) return -1;

    struct stat before;
    struct stat after;
    if (fstat(descriptor, &before) != 0 ||
        lseek(descriptor, 0, SEEK_SET) < 0)
        return -1;

    DavinciSha256 context;
    davinci_sha256_init(&context);

    uint8_t buffer[FILE_HASH_BUFFER_SIZE];
    for (;;) {
        ssize_t amount = read(descriptor, buffer, sizeof(buffer));
        if (amount == 0) break;
        if (amount < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        davinci_sha256_update(&context, buffer, (size_t)amount);
    }

    if (fstat(descriptor, &after) != 0 ||
        !file_stat_unchanged(&before, &after))
        return -1;

    davinci_sha256_final(&context, digest);
    *final_status = after;
    return 0;
}

static int open_mapped_file(const uint8_t *raw_path, size_t path_length,
                            uint32_t expected_device_major,
                            uint32_t expected_device_minor,
                            uint64_t expected_inode, int *descriptor,
                            struct stat *status) {
    if (raw_path == NULL || descriptor == NULL || status == NULL ||
        path_length == 0U || path_length >= PATH_MAX)
        return -1;

    uint8_t candidate[PATH_MAX];
    memcpy(candidate, raw_path, path_length);
    candidate[path_length] = 0;

    bool has_newline_escape = false;
    for (size_t index = 0; index + 3U < path_length; ++index) {
        if (candidate[index] == '\\' && candidate[index + 1U] == '0' &&
            candidate[index + 2U] == '1' && candidate[index + 3U] == '2') {
            has_newline_escape = true;
            break;
        }
    }

    for (unsigned attempt = 0; attempt < (has_newline_escape ? 2U : 1U);
         ++attempt) {
        if (attempt == 1U) {
            size_t source = 0;
            size_t target = 0;
            while (source < path_length) {
                if (source + 3U < path_length && candidate[source] == '\\' &&
                    candidate[source + 1U] == '0' &&
                    candidate[source + 2U] == '1' &&
                    candidate[source + 3U] == '2') {
                    candidate[target++] = '\n';
                    source += 4U;
                } else {
                    candidate[target++] = candidate[source++];
                }
            }
            candidate[target] = 0;
        }

        int opened = open((const char *)candidate, O_RDONLY | O_CLOEXEC);
        if (opened < 0) continue;
        struct stat current;
        if (fstat(opened, &current) == 0 &&
            (uint64_t)current.st_ino == expected_inode &&
            S_ISREG(current.st_mode) &&
            descriptor_matches_mapped_device(opened, expected_device_major,
                                              expected_device_minor) == 0) {
            *descriptor = opened;
            *status = current;
            return 0;
        }
        (void)close(opened);
    }
    return -1;
}

static int bytes_compare(const uint8_t *left, size_t left_length,
                         const uint8_t *right, size_t right_length) {
    size_t common = left_length < right_length ? left_length : right_length;
    for (size_t index = 0; index < common; ++index) {
        if (left[index] < right[index]) return -1;
        if (left[index] > right[index]) return 1;
    }
    if (left_length < right_length) return -1;
    if (left_length > right_length) return 1;
    return 0;
}

static void update_be32(DavinciSha256 *context, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)(value >> 24U),
        (uint8_t)(value >> 16U),
        (uint8_t)(value >> 8U),
        (uint8_t)value
    };
    davinci_sha256_update(context, bytes, sizeof(bytes));
}

static void update_be64(DavinciSha256 *context, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned index = 0; index < 8U; ++index)
        bytes[index] = (uint8_t)(value >> (56U - index * 8U));
    davinci_sha256_update(context, bytes, sizeof(bytes));
}

static bool path_has_deleted_suffix(const ParsedMapsLine *mapping) {
    static const uint8_t suffix[] = " (deleted)";
    const size_t suffix_length = sizeof(suffix) - 1U;
    if (mapping->path_length < suffix_length) return false;
    return memcmp(mapping->path + mapping->path_length - suffix_length,
                  suffix, suffix_length) == 0;
}

static void close_identity_files(MappedFileIdentity *files, size_t count) {
    if (files == NULL) return;
    for (size_t index = 0; index < count; ++index) {
        if (files[index].descriptor >= 0) {
            (void)close(files[index].descriptor);
            files[index].descriptor = -1;
        }
    }
}

int davinci_compute_process_file_identity(uint8_t executable_sha256[32],
                                          uint8_t loaded_dso_set_sha256[32],
                                          uint32_t *loaded_dso_count) {
    if (executable_sha256 == NULL || loaded_dso_set_sha256 == NULL ||
        loaded_dso_count == NULL)
        return -1;

    *loaded_dso_count = 0;
    int executable_descriptor = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (executable_descriptor < 0) return -1;
    struct stat executable_status;
    int result = hash_open_file(executable_descriptor, executable_sha256,
                                &executable_status);
    (void)executable_status;
    int close_result = close(executable_descriptor);
    if (result != 0 || close_result != 0) return -1;

    size_t allocation_size = sizeof(MappedFileIdentity) * MAX_EXECUTABLE_DSOS;
    MappedFileIdentity *files = mmap(NULL, allocation_size,
                                     PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (files == MAP_FAILED) return -1;
    for (size_t index = 0; index < MAX_EXECUTABLE_DSOS; ++index)
        files[index].descriptor = -1;

    ProcTextReader reader;
    if (proc_text_reader_init(&reader, "/proc/self/maps") != 0) {
        (void)munmap(files, allocation_size);
        return -1;
    }

    size_t file_count = 0;
    bool failed = false;
    for (;;) {
        const uint8_t *line = NULL;
        size_t line_length = 0;
        int line_result = proc_text_reader_next(&reader, &line, &line_length);
        if (line_result == 0) break;
        if (line_result < 0) {
            failed = true;
            break;
        }

        ParsedMapsLine mapping;
        if (parse_maps_line(line, line_length, &mapping) != 0) {
            failed = true;
            break;
        }
        if (mapping.permissions[2] != 'x' || mapping.inode == 0U ||
            mapping.path_length == 0U || mapping.path[0] != '/')
            continue;
        if (path_has_deleted_suffix(&mapping)) {
            failed = true;
            break;
        }

        bool duplicate = false;
        for (size_t index = 0; index < file_count; ++index) {
            if (bytes_compare(files[index].path, files[index].path_length,
                              mapping.path, mapping.path_length) == 0) {
                if (files[index].device_major != mapping.device_major ||
                    files[index].device_minor != mapping.device_minor ||
                    files[index].inode != mapping.inode) {
                    failed = true;
                }
                duplicate = true;
                break;
            }
        }
        if (failed) break;
        if (duplicate) continue;
        if (file_count >= MAX_EXECUTABLE_DSOS) {
            failed = true;
            break;
        }

        int descriptor = -1;
        struct stat opened_status;
        if (open_mapped_file(mapping.path, mapping.path_length,
                             mapping.device_major, mapping.device_minor,
                             mapping.inode, &descriptor,
                             &opened_status) != 0) {
            failed = true;
            break;
        }

        uint8_t magic[4];
        ssize_t magic_length;
        do {
            magic_length = pread(descriptor, magic, sizeof(magic), 0);
        } while (magic_length < 0 && errno == EINTR);
        if (magic_length != (ssize_t)sizeof(magic)) {
            (void)close(descriptor);
            failed = true;
            break;
        }
        if (memcmp(magic, "\x7f" "ELF", sizeof(magic)) != 0) {
            (void)close(descriptor);
            continue;
        }

        MappedFileIdentity *entry = &files[file_count];
        memcpy(entry->path, mapping.path, mapping.path_length);
        entry->path_length = mapping.path_length;
        entry->device_major = mapping.device_major;
        entry->device_minor = mapping.device_minor;
        entry->inode = mapping.inode;
        entry->descriptor = descriptor;
        if (hash_open_file(descriptor, entry->sha256,
                           &entry->final_status) != 0 ||
            !file_stat_unchanged(&opened_status, &entry->final_status)) {
            (void)close(descriptor);
            entry->descriptor = -1;
            failed = true;
            break;
        }
        ++file_count;
    }

    if (proc_text_reader_close(&reader) != 0) failed = true;
    if (!failed && file_count == 0U) failed = true;

    uint16_t order[MAX_EXECUTABLE_DSOS];
    if (!failed) {
        for (size_t index = 0; index < file_count; ++index)
            order[index] = (uint16_t)index;

        for (size_t index = 1; index < file_count; ++index) {
            uint16_t item = order[index];
            size_t destination = index;
            while (destination > 0U) {
                const MappedFileIdentity *previous = &files[order[destination - 1U]];
                const MappedFileIdentity *current = &files[item];
                if (bytes_compare(previous->path, previous->path_length,
                                  current->path, current->path_length) <= 0)
                    break;
                order[destination] = order[destination - 1U];
                --destination;
            }
            order[destination] = item;
        }

        DavinciSha256 aggregate;
        davinci_sha256_init(&aggregate);
        for (size_t position = 0; position < file_count; ++position) {
            const MappedFileIdentity *entry = &files[order[position]];
            update_be32(&aggregate, (uint32_t)entry->path_length);
            davinci_sha256_update(&aggregate, entry->path, entry->path_length);
            update_be32(&aggregate, entry->device_major);
            update_be32(&aggregate, entry->device_minor);
            update_be64(&aggregate, entry->inode);
            davinci_sha256_update(&aggregate, entry->sha256,
                                  sizeof(entry->sha256));
        }
        davinci_sha256_final(&aggregate, loaded_dso_set_sha256);

        for (size_t index = 0; index < file_count; ++index) {
            struct stat current_status;
            if (fstat(files[index].descriptor, &current_status) != 0 ||
                !file_stat_unchanged(&files[index].final_status,
                                     &current_status)) {
                failed = true;
                break;
            }
        }
    }

    close_identity_files(files, file_count);
    if (munmap(files, allocation_size) != 0) failed = true;
    if (failed) return -1;

    *loaded_dso_count = (uint32_t)file_count;
    return 0;
}

static int read_bounded_file(const char *path, uint8_t *buffer,
                             size_t capacity, size_t *length) {
    if (path == NULL || buffer == NULL || capacity == 0U || length == NULL)
        return -1;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return -1;

    size_t used = 0;
    bool overflow = false;
    for (;;) {
        if (used == capacity) {
            uint8_t extra;
            ssize_t amount;
            do {
                amount = read(descriptor, &extra, sizeof(extra));
            } while (amount < 0 && errno == EINTR);
            if (amount < 0 || amount > 0) overflow = true;
            break;
        }
        ssize_t amount = read(descriptor, buffer + used, capacity - used);
        if (amount == 0) break;
        if (amount < 0) {
            if (errno == EINTR) continue;
            overflow = true;
            break;
        }
        used += (size_t)amount;
    }
    int close_result = close(descriptor);
    if (overflow || close_result != 0) return -1;
    *length = used;
    return 0;
}

int davinci_read_self_start_ticks(uint64_t *result) {
    if (result == NULL) return -1;
    uint8_t data[4096];
    size_t length = 0;
    if (read_bounded_file("/proc/self/stat", data, sizeof(data), &length) != 0 ||
        length == 0U)
        return -1;

    size_t close_paren = length;
    while (close_paren > 0U && data[close_paren - 1U] != ')') --close_paren;
    if (close_paren == 0U) return -1;

    size_t cursor = close_paren;
    for (unsigned field = 0; field <= 19U; ++field) {
        while (cursor < length && is_map_separator(data[cursor])) ++cursor;
        size_t token_start = cursor;
        while (cursor < length && !is_map_separator(data[cursor]) &&
               data[cursor] != '\n')
            ++cursor;
        if (cursor == token_start) return -1;
        if (field == 19U) {
            size_t token_offset = token_start;
            uint64_t value = 0;
            if (parse_unsigned(data, cursor, &token_offset, 10U, &value) != 0 ||
                token_offset != cursor)
                return -1;
            *result = value;
            return 0;
        }
    }
    return -1;
}

int davinci_count_self_threads(uint32_t *result) {
    if (result == NULL) return -1;
    int descriptor = open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return -1;

    _Alignas(8) uint8_t buffer[TASK_READ_BUFFER_SIZE];
    uint32_t count = 0;
    bool failed = false;
    for (;;) {
        long amount = syscall(SYS_getdents64, descriptor, buffer,
                              sizeof(buffer));
        if (amount == 0) break;
        if (amount < 0) {
            if (errno == EINTR) continue;
            failed = true;
            break;
        }

        size_t offset = 0;
        while (offset < (size_t)amount) {
            size_t header_size = offsetof(struct linux_dirent64_local, name);
            if ((size_t)amount - offset < header_size) {
                failed = true;
                break;
            }
            struct linux_dirent64_local *entry =
                (struct linux_dirent64_local *)(void *)(buffer + offset);
            size_t record_length = entry->record_length;
            if (record_length < header_size + 1U ||
                record_length > (size_t)amount - offset) {
                failed = true;
                break;
            }
            size_t name_capacity = record_length - header_size;
            const char *terminator = memchr(entry->name, '\0', name_capacity);
            if (terminator == NULL) {
                failed = true;
                break;
            }
            size_t name_length = (size_t)(terminator - entry->name);
            if (!((name_length == 1U && entry->name[0] == '.') ||
                  (name_length == 2U && entry->name[0] == '.' &&
                   entry->name[1] == '.'))) {
                uint64_t tid = 0;
                size_t index = 0;
                for (; index < name_length; ++index) {
                    uint8_t digit = (uint8_t)entry->name[index];
                    if (digit < '0' || digit > '9' ||
                        tid > (UINT64_MAX - (digit - '0')) / 10U) {
                        failed = true;
                        break;
                    }
                    tid = tid * 10U + (digit - '0');
                }
                if (failed || index == 0U || tid == 0U || count == UINT32_MAX) {
                    failed = true;
                    break;
                }
                ++count;
            }
            offset += record_length;
        }
        if (failed) break;
    }

    int close_result = close(descriptor);
    if (failed || close_result != 0 || count == 0U) return -1;
    *result = count;
    return 0;
}

int davinci_find_mapping_range_protection(uintptr_t address,
                                          size_t length,
                                          int *protection) {
    if (protection == NULL || length == 0U ||
        address > UINTPTR_MAX - length)
        return -1;

    const uint64_t range_start = (uint64_t)address;
    const uint64_t range_end = range_start + (uint64_t)length;
    ProcTextReader reader;
    if (proc_text_reader_init(&reader, "/proc/self/maps") != 0) return -1;

    int result = -1;
    for (;;) {
        const uint8_t *line = NULL;
        size_t line_length = 0;
        int line_result = proc_text_reader_next(&reader, &line, &line_length);
        if (line_result == 0) break;
        if (line_result < 0) {
            result = -1;
            break;
        }

        ParsedMapsLine mapping;
        if (parse_maps_line(line, line_length, &mapping) != 0) {
            result = -1;
            break;
        }
        if (range_start < mapping.start || range_end > mapping.end)
            continue;

        int value = 0;
        if (mapping.permissions[0] == 'r') value |= PROT_READ;
        if (mapping.permissions[1] == 'w') value |= PROT_WRITE;
        if (mapping.permissions[2] == 'x') value |= PROT_EXEC;
        *protection = value;
        result = 0;
        break;
    }
    if (proc_text_reader_close(&reader) != 0) return -1;
    return result;
}

int davinci_find_mapping_protection(uintptr_t address, int *protection) {
    return davinci_find_mapping_range_protection(address, 1U, protection);
}

static int mapping_protection_bits(const ParsedMapsLine *mapping) {
    int value = 0;
    if (mapping->permissions[0] == 'r') value |= PROT_READ;
    if (mapping->permissions[1] == 'w') value |= PROT_WRITE;
    if (mapping->permissions[2] == 'x') value |= PROT_EXEC;
    return value;
}

int davinci_find_mapping_identity(uintptr_t address,
                                  DavinciMappingIdentity *identity) {
    if (identity == NULL) return -1;

    ProcTextReader reader;
    if (proc_text_reader_init(&reader, "/proc/self/maps") != 0) return -1;

    int result = -1;
    for (;;) {
        const uint8_t *line = NULL;
        size_t line_length = 0U;
        int line_result = proc_text_reader_next(&reader, &line, &line_length);
        if (line_result == 0) break;
        if (line_result < 0) {
            result = -1;
            break;
        }

        ParsedMapsLine mapping;
        if (parse_maps_line(line, line_length, &mapping) != 0) {
            result = -1;
            break;
        }
        if ((uint64_t)address < mapping.start ||
            (uint64_t)address >= mapping.end)
            continue;

        memset(identity, 0, sizeof(*identity));
        identity->start = (uintptr_t)mapping.start;
        identity->end = (uintptr_t)mapping.end;
        identity->protection = (uint32_t)mapping_protection_bits(&mapping);
        identity->device_major = mapping.device_major;
        identity->device_minor = mapping.device_minor;
        identity->inode = mapping.inode;
        identity->path_length = mapping.path_length;
        if (mapping.path_length != 0U)
            memcpy(identity->path, mapping.path, mapping.path_length);
        result = 0;
        break;
    }

    if (proc_text_reader_close(&reader) != 0) return -1;
    return result;
}

static int align_address_up(uintptr_t value, size_t alignment,
                            uintptr_t *aligned) {
    if (aligned == NULL || alignment == 0U ||
        (alignment & (alignment - 1U)) != 0U)
        return -1;
    const uintptr_t mask = (uintptr_t)alignment - 1U;
    if (value > UINTPTR_MAX - mask) return -1;
    *aligned = (value + mask) & ~mask;
    return 0;
}

int davinci_find_unmapped_range(uintptr_t range_start, uintptr_t range_end,
                                size_t length, size_t alignment,
                                uintptr_t *candidate) {
    if (candidate == NULL || range_end <= range_start || length == 0U ||
        alignment == 0U || (alignment & (alignment - 1U)) != 0U ||
        length > UINTPTR_MAX - range_start)
        return -1;

    uintptr_t cursor = 0U;
    if (align_address_up(range_start, alignment, &cursor) != 0 ||
        cursor >= range_end || length > range_end - cursor)
        return -1;

    ProcTextReader reader;
    if (proc_text_reader_init(&reader, "/proc/self/maps") != 0) return -1;

    bool failed = false;
    uintptr_t found = 0U;
    bool gap_found = false;
    for (;;) {
        const uint8_t *line = NULL;
        size_t line_length = 0U;
        int line_result = proc_text_reader_next(&reader, &line, &line_length);
        if (line_result == 0) break;
        if (line_result < 0) {
            failed = true;
            break;
        }

        ParsedMapsLine mapping;
        if (parse_maps_line(line, line_length, &mapping) != 0) {
            failed = true;
            break;
        }
        if (gap_found || mapping.end <= (uint64_t)cursor ||
            mapping.start >= (uint64_t)range_end)
            continue;

        if (mapping.start > (uint64_t)cursor &&
            mapping.start - (uint64_t)cursor >= (uint64_t)length) {
            found = cursor;
            gap_found = true;
            continue;
        }

        if (mapping.end > (uint64_t)cursor) {
            if (mapping.end > (uint64_t)UINTPTR_MAX ||
                align_address_up((uintptr_t)mapping.end, alignment,
                                 &cursor) != 0 ||
                cursor >= range_end || length > range_end - cursor) {
                failed = true;
                break;
            }
        }
    }

    if (proc_text_reader_close(&reader) != 0 || failed) return -1;

    if (!gap_found) {
        if (cursor >= range_end || length > range_end - cursor)
            return -1;
        found = cursor;
    }

    if ((found & ((uintptr_t)alignment - 1U)) != 0U ||
        found < range_start || found >= range_end ||
        length > range_end - found)
        return -1;

    *candidate = found;
    return 0;
}

#ifdef DAVINCI_TARGET_IDENTITY_TEST
int davinci_test_parse_maps_line(const uint8_t *line, size_t length,
                                 uint64_t *start, uint64_t *end,
                                 uint32_t *device_major,
                                 uint32_t *device_minor, uint64_t *inode,
                                 uint8_t *path, size_t path_capacity,
                                 size_t *path_length) {
    ParsedMapsLine parsed;
    if (parse_maps_line(line, length, &parsed) != 0 ||
        path_capacity < parsed.path_length)
        return -1;
    *start = parsed.start;
    *end = parsed.end;
    *device_major = parsed.device_major;
    *device_minor = parsed.device_minor;
    *inode = parsed.inode;
    *path_length = parsed.path_length;
    if (parsed.path_length != 0U)
        memcpy(path, parsed.path, parsed.path_length);
    return 0;
}

int davinci_test_parse_mountinfo_line(const uint8_t *line, size_t length,
                                      uint64_t *mount_id,
                                      uint32_t *device_major,
                                      uint32_t *device_minor) {
    return parse_mountinfo_line(line, length, mount_id,
                                device_major, device_minor);
}

int davinci_test_lookup_mount_device_text(const uint8_t *text, size_t length,
                                          uint64_t mount_id,
                                          uint32_t *device_major,
                                          uint32_t *device_minor) {
    return lookup_mount_device_text(text, length, mount_id,
                                    device_major, device_minor);
}
#endif
