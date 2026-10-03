#ifndef NOSLEEP_UPDATER_COMMAND_LINE_H
#define NOSLEEP_UPDATER_COMMAND_LINE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wchar.h>

#define UPDATER_MAX_WINDOWS_COMMAND_LINE_CHARS 32767
#define UPDATER_INTERNAL_RELAUNCH_OPTION "--nosleep-internal-update-relaunch"
#define UPDATER_INTERNAL_RELAUNCH_OPTION_W L"--nosleep-internal-update-relaunch"

static inline bool updater_add_argument_size(size_t* size, size_t amount) {
    if (*size > (size_t)-1 - amount) return false;
    *size += amount;
    return true;
}

static inline bool updater_argument_needs_quotes(const wchar_t* argument) {
    if (*argument == L'\0') return true;
    for (const wchar_t* cursor = argument; *cursor; ++cursor) {
        if (*cursor == L' ' || *cursor == L'\t' || *cursor == L'"') return true;
    }
    return false;
}

// Build a complete CreateProcess command line while preserving parsed Windows argv.
static inline wchar_t* updater_build_windows_command_line(int argc,
                                                           wchar_t* const argv[]) {
    if (argc < 0 || (argc > 0 && !argv)) return NULL;

    size_t length = 0;
    for (int i = 0; i < argc; ++i) {
        if (!argv[i]) return NULL;
        if (i > 0 && !updater_add_argument_size(&length, 1)) return NULL;
        size_t argument_length = wcslen(argv[i]);
        if (updater_argument_needs_quotes(argv[i])) {
            if (argument_length > ((size_t)-1 - 2) / 2 ||
                !updater_add_argument_size(&length, argument_length * 2 + 2)) return NULL;
        } else if (!updater_add_argument_size(&length, argument_length)) {
            return NULL;
        }
    }

    if (length == (size_t)-1 || length + 1 > (size_t)-1 / sizeof(wchar_t)) return NULL;
    wchar_t* result = (wchar_t*)malloc((length + 1) * sizeof(wchar_t));
    if (!result) return NULL;

    wchar_t* output = result;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) *output++ = L' ';
        bool quote = updater_argument_needs_quotes(argv[i]);
        if (quote) *output++ = L'"';

        size_t backslashes = 0;
        for (const wchar_t* cursor = argv[i]; ; ++cursor) {
            if (*cursor == L'\\') {
                ++backslashes;
                continue;
            }

            size_t backslash_count = backslashes;
            if (quote && (*cursor == L'"' || *cursor == L'\0')) backslash_count *= 2;
            if (quote && *cursor == L'"') ++backslash_count;
            for (size_t j = 0; j < backslash_count; ++j) *output++ = L'\\';
            if (*cursor == L'\0') break;
            *output++ = *cursor;
            backslashes = 0;
        }

        if (quote) *output++ = L'"';
    }
    *output = L'\0';
    return result;
}

#endif
