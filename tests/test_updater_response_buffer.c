#include "../src/updater_response_buffer.h"

#include <stdint.h>
#include <stdio.h>

static int test_rejects_max_content_length_before_adding_terminator(void) {
    uint32_t buffer_size = 0;
    if (updater_initial_response_buffer_size(UINT32_MAX, &buffer_size)) {
        fprintf(stderr, "maximum Content-Length should be rejected before size addition\n");
        return 1;
    }
    return 0;
}

static int test_initial_buffer_size_stays_within_safe_growth_limit(void) {
    uint32_t buffer_size = 0;
    uint32_t largest_safe_content_length = UINT32_MAX / 2u - 1u;
    if (!updater_initial_response_buffer_size(largest_safe_content_length,
                                              &buffer_size) ||
        buffer_size != UINT32_MAX / 2u) {
        fprintf(stderr, "largest safe Content-Length should reserve a growable buffer\n");
        return 1;
    }

    if (updater_initial_response_buffer_size(UINT32_MAX / 2u, &buffer_size)) {
        fprintf(stderr, "Content-Length requiring an overflowing first growth should be rejected\n");
        return 1;
    }
    if (updater_initial_response_buffer_size(UINT32_MAX / 2u + 1u,
                                              &buffer_size)) {
        fprintf(stderr, "2 GiB Content-Length should be rejected before allocation\n");
        return 1;
    }
    return 0;
}

static int test_capacity_growth_rejects_dword_overflow(void) {
    uint32_t next_size = 0;
    if (!updater_response_buffer_next_size(UINT32_MAX / 2u, &next_size) ||
        next_size != UINT32_MAX - 1u) {
        fprintf(stderr, "largest safely doubleable capacity should double without wrapping\n");
        return 1;
    }

    if (updater_response_buffer_next_size(UINT32_MAX / 2u + 1u, &next_size) ||
        updater_response_buffer_next_size(UINT32_MAX, &next_size)) {
        fprintf(stderr, "capacity doubling past the DWORD limit should be rejected\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_rejects_max_content_length_before_adding_terminator();
    failures += test_initial_buffer_size_stays_within_safe_growth_limit();
    failures += test_capacity_growth_rejects_dword_overflow();
    printf("updater response buffer tests: %d passed, %d failed\n",
           3 - failures, failures);
    return failures == 0 ? 0 : 1;
}
