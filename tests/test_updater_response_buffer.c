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

int main(void) {
    int failures = test_rejects_max_content_length_before_adding_terminator();
    printf("updater response buffer tests: %d passed, %d failed\n",
           1 - failures, failures);
    return failures == 0 ? 0 : 1;
}
