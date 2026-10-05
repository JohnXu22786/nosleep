#include "updater_response_read.h"

#include "updater_response_buffer.h"

#include <stdlib.h>
#include <string.h>

UpdaterResponseReadResult updater_read_response_body(UpdaterStreamRead read_data,
                                                     void* read_context,
                                                     uint32_t content_length,
                                                     char** response_out) {
    if (!response_out) return UPDATER_RESPONSE_READ_INVALID_ARGUMENT;
    *response_out = NULL;
    if (!read_data) return UPDATER_RESPONSE_READ_INVALID_ARGUMENT;

    uint32_t buffer_size = 0;
    if (!updater_initial_response_buffer_size(content_length, &buffer_size)) {
        return UPDATER_RESPONSE_READ_TOO_LARGE;
    }

    char* response = (char*)malloc(buffer_size);
    if (!response) return UPDATER_RESPONSE_READ_OUT_OF_MEMORY;
    memset(response, 0, buffer_size);

    uint32_t total_read = 0;
    for (;;) {
        size_t bytes_read = 0;
        bool read_ok = read_data(read_context, response + total_read,
                                 buffer_size - total_read - 1u, &bytes_read);
        if (!read_ok) {
            free(response);
            return UPDATER_RESPONSE_READ_FAILED;
        }
        if (bytes_read == 0) break;

        if (bytes_read > buffer_size - total_read - 1u) {
            free(response);
            return UPDATER_RESPONSE_READ_FAILED;
        }
        total_read += (uint32_t)bytes_read;
        if (total_read >= buffer_size - 1u) {
            uint32_t new_buffer_size = 0;
            if (!updater_response_buffer_next_size(buffer_size, &new_buffer_size)) {
                free(response);
                return UPDATER_RESPONSE_READ_TOO_LARGE;
            }
            char* new_response = (char*)realloc(response, new_buffer_size);
            if (!new_response) {
                free(response);
                return UPDATER_RESPONSE_READ_OUT_OF_MEMORY_GROWTH;
            }
            response = new_response;
            memset(response + total_read, 0, new_buffer_size - total_read);
            buffer_size = new_buffer_size;
        }
    }

    response[total_read] = '\0';
    *response_out = response;
    return UPDATER_RESPONSE_READ_OK;
}
