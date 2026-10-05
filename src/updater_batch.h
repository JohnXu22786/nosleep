#ifndef NOSLEEP_UPDATER_BATCH_H
#define NOSLEEP_UPDATER_BATCH_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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

// Quote an already escaped path in the updater's failure-message echo line.
static inline char* updater_format_batch_error_path_line(const char* escaped_path) {
    if (!escaped_path) return NULL;

    const char prefix[] = "    echo   \"";
    const char suffix[] = "\"\r\n";
    size_t path_length = strlen(escaped_path);
    size_t fixed_size = sizeof(prefix) - 1 + sizeof(suffix);
    if (path_length > (size_t)-1 - fixed_size) return NULL;

    char* line = (char*)malloc(path_length + fixed_size);
    if (!line) return NULL;

    memcpy(line, prefix, sizeof(prefix) - 1);
    memcpy(line + sizeof(prefix) - 1, escaped_path, path_length);
    memcpy(line + sizeof(prefix) - 1 + path_length, suffix, sizeof(suffix));
    return line;
}

#endif
