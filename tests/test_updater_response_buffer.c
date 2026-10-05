#include "../src/updater_response_buffer.h"

#include <stdint.h>
#include <stdio.h>

static int test_initial_buffer_respects_response_limit(void) {
    uint32_t buffer_size = 0;
    if (!updater_initial_response_buffer_size(UPDATER_MAX_RESPONSE_SIZE,
                                              &buffer_size) ||
        buffer_size != UPDATER_MAX_RESPONSE_BUFFER_SIZE) {
        fprintf(stderr, "1 MiB Content-Length should reserve space for a terminator\n");
        return 1;
    }
    if (updater_initial_response_buffer_size(UPDATER_MAX_RESPONSE_SIZE + 1u,
                                              &buffer_size)) {
        fprintf(stderr, "Content-Length above 1 MiB should be rejected before allocation\n");
        return 1;
    }
    return 0;
}

static int test_capacity_growth_stops_at_response_limit(void) {
    uint32_t next_size = 0;
    if (!updater_response_buffer_next_size(
            UPDATER_MAX_RESPONSE_BUFFER_SIZE / 2u + 1u, &next_size) ||
        next_size != UPDATER_MAX_RESPONSE_BUFFER_SIZE) {
        fprintf(stderr, "buffer growth should clamp at the 1 MiB limit plus terminator\n");
        return 1;
    }

    if (updater_response_buffer_next_size(UPDATER_MAX_RESPONSE_BUFFER_SIZE,
                                          &next_size) ||
        updater_response_buffer_next_size(UINT32_MAX, &next_size)) {
        fprintf(stderr, "buffer growth beyond the response limit should be rejected\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_initial_buffer_respects_response_limit();
    failures += test_capacity_growth_stops_at_response_limit();
    printf("updater response buffer tests: %d passed, %d failed\n",
           2 - failures, failures);
    return failures == 0 ? 0 : 1;
}
