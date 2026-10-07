#ifndef NOSLEEP_UPDATER_STREAM_H
#define NOSLEEP_UPDATER_STREAM_H

#include <stdbool.h>
#include <stddef.h>

typedef bool (*UpdaterStreamRead)(void* context, void* buffer, size_t capacity,
                                 size_t* bytes_read);
typedef bool (*UpdaterStreamWrite)(void* context, const void* buffer,
                                   size_t bytes_to_write, size_t* bytes_written);
typedef bool (*UpdaterStreamProgress)(void* context, size_t bytes_copied);

typedef enum {
    UPDATER_STREAM_OK,
    UPDATER_STREAM_READ_FAILED,
    UPDATER_STREAM_WRITE_FAILED,
    UPDATER_STREAM_SIZE_REJECTED,
    UPDATER_STREAM_INTERRUPTED
} UpdaterStreamResult;

static inline UpdaterStreamResult updater_copy_stream_result(UpdaterStreamRead read_data, void* read_context,
                                       UpdaterStreamWrite write_data, void* write_context,
                                       void* buffer, size_t buffer_capacity, size_t max_bytes,
                                       UpdaterStreamProgress report_progress,
                                       void* progress_context) {
    if (!read_data || !write_data || !buffer || buffer_capacity == 0) return UPDATER_STREAM_INTERRUPTED;

    bool wrote_data = false;
    size_t total_bytes = 0;
    for (;;) {
        size_t bytes_read = 0;
        if (!read_data(read_context, buffer, buffer_capacity, &bytes_read)) return UPDATER_STREAM_READ_FAILED;
        if (bytes_read == 0) return wrote_data ? UPDATER_STREAM_OK : UPDATER_STREAM_READ_FAILED;
        if (bytes_read > buffer_capacity || bytes_read > max_bytes - total_bytes) return UPDATER_STREAM_SIZE_REJECTED;

        size_t bytes_written = 0;
        if (!write_data(write_context, buffer, bytes_read, &bytes_written) ||
            bytes_written != bytes_read) {
            return UPDATER_STREAM_WRITE_FAILED;
        }
        total_bytes += bytes_read;
        wrote_data = true;
        if (report_progress && !report_progress(progress_context, total_bytes)) return UPDATER_STREAM_INTERRUPTED;
    }
}

static inline bool updater_copy_stream(UpdaterStreamRead read_data, void* read_context,
                                       UpdaterStreamWrite write_data, void* write_context,
                                       void* buffer, size_t buffer_capacity, size_t max_bytes,
                                       UpdaterStreamProgress report_progress,
                                       void* progress_context) {
    return updater_copy_stream_result(read_data, read_context, write_data, write_context,
                                      buffer, buffer_capacity, max_bytes,
                                      report_progress, progress_context) == UPDATER_STREAM_OK;
}

#endif
