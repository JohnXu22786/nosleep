#ifndef NOSLEEP_UPDATER_BATCH_H
#define NOSLEEP_UPDATER_BATCH_H

#include <stddef.h>
#include <stdlib.h>

// Batch files require percent signs to be doubled to preserve them literally.
static inline char* updater_escape_batch_path(const char* path) {
    if (!path) return NULL;

    size_t escaped_size = 0;
    for (const char* cursor = path; *cursor; ++cursor) {
        size_t added_size = *cursor == '%' ? 2 : 1;
        if (escaped_size > (size_t)-1 - added_size) return NULL;
        escaped_size += added_size;
    }
    if (escaped_size == (size_t)-1) return NULL;

    char* escaped = (char*)malloc(escaped_size + 1);
    if (!escaped) return NULL;

    char* output = escaped;
    for (const char* cursor = path; *cursor; ++cursor) {
        *output++ = *cursor;
        if (*cursor == '%') *output++ = '%';
    }
    *output = '\0';
    return escaped;
}

#endif
