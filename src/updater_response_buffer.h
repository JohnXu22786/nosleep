#ifndef NOSLEEP_UPDATER_RESPONSE_BUFFER_H
#define NOSLEEP_UPDATER_RESPONSE_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

static inline bool updater_initial_response_buffer_size(uint32_t content_length,
                                                        uint32_t* buffer_size) {
    // The full-buffer path doubles even when Content-Length exactly matches.
    if (!buffer_size || content_length >= UINT32_MAX / 2u) return false;
    *buffer_size = content_length > 0 ? content_length + 1u : 8192u;
    return true;
}

static inline bool updater_response_buffer_next_size(uint32_t current_size,
                                                     uint32_t* next_size) {
    if (!next_size || current_size == 0 || current_size > UINT32_MAX / 2u) {
        return false;
    }
    *next_size = current_size * 2u;
    return true;
}

#endif
