#ifndef NOSLEEP_UPDATER_RESPONSE_READ_H
#define NOSLEEP_UPDATER_RESPONSE_READ_H

#include "updater_stream.h"

#include <stdint.h>

typedef enum {
    UPDATER_RESPONSE_READ_OK,
    UPDATER_RESPONSE_READ_INVALID_ARGUMENT,
    UPDATER_RESPONSE_READ_TOO_LARGE,
    UPDATER_RESPONSE_READ_OUT_OF_MEMORY,
    UPDATER_RESPONSE_READ_OUT_OF_MEMORY_GROWTH,
    UPDATER_RESPONSE_READ_FAILED
} UpdaterResponseReadResult;

UpdaterResponseReadResult updater_read_response_body(UpdaterStreamRead read_data,
                                                     void* read_context,
                                                     uint32_t content_length,
                                                     char** response);

#endif
