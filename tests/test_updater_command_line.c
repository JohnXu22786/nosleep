// Regression test for preserving the original Windows argv across updater restart.
#include "updater_command_line.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static void expect_command_line(const wchar_t* expected, int argc, wchar_t* argv[]) {
    wchar_t* command_line = updater_build_windows_command_line(argc, argv);
    if (!command_line || wcscmp(command_line, expected) != 0) {
        fwprintf(stderr, L"expected: %ls\nactual:   %ls\n", expected,
                 command_line ? command_line : L"(null)");
        free(command_line);
        exit(1);
    }
    free(command_line);
}

int main(void) {
    wchar_t* simple_argv[] = {
        L"C:\\Program Files\\nosleep.exe", L"--duration", L"25",
        L"", L"two words"
    };
    expect_command_line(L"\"C:\\Program Files\\nosleep.exe\" --duration 25 \"\" \"two words\"",
                        5, simple_argv);

    wchar_t* quoted_argv[] = {
        L"nosleep.exe", L"quote\"inside", L"slash\\\"quote", L"trailing\\"
    };
    expect_command_line(L"nosleep.exe \"quote\\\"inside\" \"slash\\\\\\\"quote\" trailing\\",
                        4, quoted_argv);

    wchar_t* trailing_backslash_argv[] = {
        L"nosleep.exe", L"trailing slash \\"
    };
    expect_command_line(L"nosleep.exe \"trailing slash \\\\" L"\"",
                        2, trailing_backslash_argv);

    wchar_t* no_args_argv[] = {L"nosleep.exe"};
    expect_command_line(L"nosleep.exe", 1, no_args_argv);
    expect_command_line(L"", 0, NULL);

    enum { repeated_argument_count = 3275 };
    wchar_t* repeated_argv[repeated_argument_count + 1];
    repeated_argv[0] = L"nosleep.exe";
    for (size_t i = 1; i <= repeated_argument_count; ++i) {
        repeated_argv[i] = L"--verbose";
    }
    wchar_t* repeated_arguments = updater_build_windows_command_line(
        repeated_argument_count + 1, repeated_argv);
    size_t expected_length = repeated_argument_count * wcslen(L"--verbose") +
                             repeated_argument_count + wcslen(repeated_argv[0]);
    if (!repeated_arguments || expected_length >= UPDATER_MAX_WINDOWS_COMMAND_LINE_CHARS ||
        wcslen(repeated_arguments) != expected_length ||
        wcsncmp(repeated_arguments, L"nosleep.exe --verbose --verbose",
                wcslen(L"nosleep.exe --verbose --verbose")) != 0) {
        fwprintf(stderr, L"repeated CLI arguments were unexpectedly expanded\n");
        free(repeated_arguments);
        return 1;
    }
    free(repeated_arguments);

    puts("updater command-line argument serialization test passed");
    return 0;
}
