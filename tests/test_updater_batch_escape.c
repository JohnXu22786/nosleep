// Regression test for percent signs in paths written to an updater batch file.
#include "updater_batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char path[] = "C:\\Users\\Alice\\%TEMP%\\100% complete\\nosleep.exe";
    const char expected[] = "C:\\Users\\Alice\\%%TEMP%%\\100%% complete\\nosleep.exe";
    const char ordinary_path[] = "C:\\Program Files\\nosleep.exe";

    char* escaped = updater_escape_batch_path(path);
    if (!escaped || strcmp(escaped, expected) != 0) {
        fprintf(stderr, "percent signs were not escaped in the batch path\n");
        free(escaped);
        return 1;
    }
    free(escaped);

    escaped = updater_escape_batch_path(ordinary_path);
    if (!escaped || strcmp(escaped, ordinary_path) != 0) {
        fprintf(stderr, "path without percent signs changed unexpectedly\n");
        free(escaped);
        return 1;
    }
    free(escaped);

    puts("updater batch path escaping test passed");
    return 0;
}
