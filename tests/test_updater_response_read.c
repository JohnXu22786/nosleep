#include "updater_logic.h"
#include "updater_response_buffer.h"
#include "updater_response_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* data;
    size_t length;
    int calls;
    bool fail_after_data;
} PartialThenFailedRead;

static bool read_partial_then_fail(void* context, void* buffer, size_t capacity,
                                   size_t* bytes_read) {
    PartialThenFailedRead* state = (PartialThenFailedRead*)context;
    *bytes_read = 0;
    if (state->calls++ != 0) return !state->fail_after_data;
    if (state->length > capacity) return false;
    memcpy(buffer, state->data, state->length);
    *bytes_read = state->length;
    return true;
}

typedef struct {
    size_t length;
    size_t bytes_returned;
    size_t largest_capacity;
} GeneratedResponse;

static bool read_generated_response(void* context, void* buffer, size_t capacity,
                                    size_t* bytes_read) {
    GeneratedResponse* state = (GeneratedResponse*)context;
    *bytes_read = 0;
    if (capacity > state->largest_capacity) state->largest_capacity = capacity;
    if (state->bytes_returned == state->length) return true;

    size_t remaining = state->length - state->bytes_returned;
    size_t amount = remaining < capacity ? remaining : capacity;
    if (amount == 0) return true;
    memset(buffer, 'x', amount);
    state->bytes_returned += amount;
    *bytes_read = amount;
    return true;
}

static bool read_nothing(void* context, void* buffer, size_t capacity,
                         size_t* bytes_read) {
    int* calls = (int*)context;
    (void)buffer;
    (void)capacity;
    *bytes_read = 0;
    ++*calls;
    return true;
}

static int test_read_failure_after_valid_tag_json_fails_update_check(void) {
    static const char response_json[] =
        "{\"tag_name\":\"v9.9.9\",\"assets\":[{\"browser_download_url\":"
        "\"https://github.com/JohnXu22786/nosleep/releases/download/v9.9.9/nosleep-9.9.9.exe\"}]}";
    PartialThenFailedRead state = {
        response_json, sizeof(response_json) - 1u, 0, true
    };
    char* response = NULL;

    if (updater_read_response_body(read_partial_then_fail, &state, 0,
                                   &response) != UPDATER_RESPONSE_READ_FAILED) {
        fprintf(stderr, "read failure after partial response should fail the update check\n");
        free(response);
        return 1;
    }
    if (response != NULL) {
        fprintf(stderr, "failed response read should not return parsable partial data\n");
        free(response);
        return 1;
    }

    UpdateInfo info;
    if (!updater_parse_response(response_json, &info) || !info.update_available) {
        fprintf(stderr, "release with an EXE asset should be accepted by the parser\n");
        return 1;
    }

    state.calls = 0;
    state.fail_after_data = false;
    if (updater_read_response_body(read_partial_then_fail, &state, 0,
                                   &response) != UPDATER_RESPONSE_READ_OK ||
        !response || strcmp(response, response_json) != 0) {
        fprintf(stderr, "clean EOF should still return the complete response body\n");
        free(response);
        return 1;
    }
    free(response);
    return 0;
}

static int test_declared_response_over_limit_is_rejected_before_read(void) {
    int calls = 0;
    char* response = NULL;
    UpdaterResponseReadResult result = updater_read_response_body(
        read_nothing, &calls, UPDATER_MAX_RESPONSE_SIZE + 1u, &response);
    if (result != UPDATER_RESPONSE_READ_TOO_LARGE || calls != 0 || response) {
        fprintf(stderr, "declared response over 1 MiB should be rejected before reading\n");
        free(response);
        return 1;
    }
    return 0;
}

static int test_unknown_length_response_is_bounded_at_limit(void) {
    GeneratedResponse exact_limit = {
        UPDATER_MAX_RESPONSE_SIZE, 0, 0
    };
    char* response = NULL;
    if (updater_read_response_body(read_generated_response, &exact_limit, 0,
                                   &response) != UPDATER_RESPONSE_READ_OK ||
        !response || exact_limit.bytes_returned != UPDATER_MAX_RESPONSE_SIZE ||
        strlen(response) != UPDATER_MAX_RESPONSE_SIZE) {
        fprintf(stderr, "unknown-length response exactly at 1 MiB should succeed\n");
        free(response);
        return 1;
    }
    free(response);

    GeneratedResponse oversized = {
        UPDATER_MAX_RESPONSE_SIZE + 1u, 0, 0
    };
    response = NULL;
    if (updater_read_response_body(read_generated_response, &oversized, 0,
                                   &response) != UPDATER_RESPONSE_READ_TOO_LARGE ||
        response || oversized.bytes_returned != UPDATER_MAX_RESPONSE_SIZE + 1u) {
        fprintf(stderr, "unknown-length response beyond 1 MiB should be rejected\n");
        free(response);
        return 1;
    }
    if (oversized.largest_capacity > UPDATER_MAX_RESPONSE_SIZE) {
        fprintf(stderr, "response reader should never request more than the 1 MiB limit\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = test_read_failure_after_valid_tag_json_fails_update_check();
    failures += test_declared_response_over_limit_is_rejected_before_read();
    failures += test_unknown_length_response_is_bounded_at_limit();
    printf("updater response read tests: %d passed, %d failed\n",
           3 - failures, failures);
    return failures == 0 ? 0 : 1;
}
