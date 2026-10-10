#include "updater_response_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void* test_realloc(void* pointer, size_t size) {
    (void)pointer;
    (void)size;
    return NULL;
}

typedef struct {
    const char* data;
    size_t length;
    int calls;
} ExactLengthResponse;

static bool read_exact_length_response(void* context, void* buffer,
                                       size_t capacity, size_t* bytes_read) {
    ExactLengthResponse* state = (ExactLengthResponse*)context;
    *bytes_read = 0;
    if (state->calls++ != 0 || state->length > capacity) return false;
    memcpy(buffer, state->data, state->length);
    *bytes_read = state->length;
    return true;
}

static int test_complete_declared_body_does_not_need_growth(void) {
    static const char body[] = "body";
    ExactLengthResponse state = {body, sizeof(body) - 1u, 0};
    char* response = NULL;
    UpdaterResponseReadResult result = updater_read_response_body(
        read_exact_length_response, &state, (uint32_t)state.length, &response);

    if (result != UPDATER_RESPONSE_READ_OK || !response ||
        strcmp(response, body) != 0 || state.calls != 1) {
        fprintf(stderr, "complete Content-Length body should succeed without buffer growth\n");
        free(response);
        return 1;
    }

    free(response);
    return 0;
}

int main(void) {
    int failures = test_complete_declared_body_does_not_need_growth();
    printf("updater response Content-Length tests: %d passed, %d failed\n",
           1 - failures, failures);
    return failures == 0 ? 0 : 1;
}
