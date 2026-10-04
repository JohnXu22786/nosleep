#include "../src/updater_temp_path.h"

#include <stdio.h>
#include <string.h>

static int test_temp_path_overflow_is_rejected_and_fitting_path_is_preserved(void) {
    enum { MAX_PATH_LENGTH = 260 };
    char temp_dir[231];
    char path[MAX_PATH_LENGTH];
    static const char unique_name[] = "nosleep_update_12345_67890.exe";
    static const char expected[] = "C:\\Temp\\nosleep_update_12345_67890.exe";

    memset(temp_dir, 'a', sizeof(temp_dir) - 1);
    temp_dir[sizeof(temp_dir) - 2] = '\\';
    temp_dir[sizeof(temp_dir) - 1] = '\0';

    if (updater_build_temp_path(path, sizeof(path), temp_dir, unique_name)) {
        fprintf(stderr, "temp path exceeding MAX_PATH was accepted\n");
        return 1;
    }
    if (path[0] != '\0') {
        fprintf(stderr, "failed temp path left a truncated path in the output\n");
        return 1;
    }

    if (!updater_build_temp_path(path, sizeof(path), "C:\\Temp\\",
                                 "nosleep_update_12345_67890.exe")) {
        fprintf(stderr, "valid temp path was rejected\n");
        return 1;
    }
    if (strcmp(path, expected) != 0) {
        fprintf(stderr, "valid temp path did not preserve the full unique name\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = test_temp_path_overflow_is_rejected_and_fitting_path_is_preserved();
    printf("updater temp path tests: %d passed, %d failed\n",
           1 - failures, failures);
    return failures == 0 ? 0 : 1;
}
