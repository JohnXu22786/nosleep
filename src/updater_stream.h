#ifndef NOSLEEP_UPDATER_STREAM_H
#define NOSLEEP_UPDATER_STREAM_H

#include <stdbool.h>
#include <stddef.h>

typedef bool (*UpdaterStreamRead)(void* context, void* buffer, size_t capacity,
                                 size_t* bytes_read);
typedef bool (*UpdaterStreamWrite)(void* context, const void* buffer,
                                   size_t bytes_to_write, size_t* bytes_written);

static inline bool updater_copy_stream(UpdaterStreamRead read_data, void* read_context,
                                       UpdaterStreamWrite write_data, void* write_context,
                                       void* buffer, size_t buffer_capacity) {
    if (!read_data || !write_data || !buffer || buffer_capacity == 0) return false;

    bool wrote_data = false;
    for (;;) {
        size_t bytes_read = 0;
        if (!read_data(read_context, buffer, buffer_capacity, &bytes_read)) return false;
        if (bytes_read == 0) return wrote_data;
        if (bytes_read > buffer_capacity) return false;

        size_t bytes_written = 0;
        if (!write_data(write_context, buffer, bytes_read, &bytes_written) ||
            bytes_written != bytes_read) {
            return false;
        }
        wrote_data = true;
    }
}

#endif
