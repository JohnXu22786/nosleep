#ifndef NOSLEEP_UPDATER_RESPONSE_BUFFER_H
#define NOSLEEP_UPDATER_RESPONSE_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

static inline bool updater_initial_response_buffer_size(uint32_t content_length,
                                                        uint32_t* buffer_size) {
    if (content_length == UINT32_MAX) return false;
    *buffer_size = content_length > 0 ? content_length + 1u : 8192u;
    return true;
}

#endif
