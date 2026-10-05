#ifndef NOSLEEP_UPDATER_RESPONSE_BUFFER_H
#define NOSLEEP_UPDATER_RESPONSE_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

// Release metadata, including GitHub's release-notes body, fits comfortably below 1 MiB.
#define UPDATER_MAX_RESPONSE_SIZE (1024u * 1024u)
#define UPDATER_MAX_RESPONSE_BUFFER_SIZE (UPDATER_MAX_RESPONSE_SIZE + 1u)

static inline bool updater_initial_response_buffer_size(uint32_t content_length,
                                                        uint32_t* buffer_size) {
    if (!buffer_size || content_length > UPDATER_MAX_RESPONSE_SIZE) return false;
    *buffer_size = content_length > 0 ? content_length + 1u : 8192u;
    return true;
}

static inline bool updater_response_buffer_next_size(uint32_t current_size,
                                                     uint32_t* next_size) {
    if (!next_size || current_size == 0 ||
        current_size >= UPDATER_MAX_RESPONSE_BUFFER_SIZE) {
        return false;
    }
    if (current_size > UPDATER_MAX_RESPONSE_BUFFER_SIZE / 2u) {
        *next_size = UPDATER_MAX_RESPONSE_BUFFER_SIZE;
    } else {
        *next_size = current_size * 2u;
    }
    return true;
}

#endif
