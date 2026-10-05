#include "updater_logic.h"
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

static int test_read_failure_after_valid_tag_json_fails_update_check(void) {
    static const char response_json[] = "{\"tag_name\":\"v9.9.9\"}";
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
        fprintf(stderr, "regression fixture must be accepted by the tag fallback parser\n");
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

int main(void) {
    int failures = test_read_failure_after_valid_tag_json_fails_update_check();
    printf("updater response read tests: %d passed, %d failed\n",
           1 - failures, failures);
    return failures == 0 ? 0 : 1;
}
