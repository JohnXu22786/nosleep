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

    const char injected_path[] = "C:\\Temp\\download & run\\100% complete\\nosleep.exe";
    const char expected_error_line[] =
        "    echo   \"C:\\Temp\\download & run\\100%% complete\\nosleep.exe\"\r\n";
    escaped = updater_escape_batch_path(injected_path);
    char* error_line = escaped ? updater_format_batch_error_path_line(escaped) : NULL;
    if (!error_line || strcmp(error_line, expected_error_line) != 0) {
        fprintf(stderr, "downloaded path was not quoted in the batch error message\n");
        free(escaped);
        free(error_line);
        return 1;
    }
    free(escaped);
    free(error_line);

    puts("updater batch path escaping test passed");
    return 0;
}
