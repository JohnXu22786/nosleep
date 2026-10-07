#ifndef NOSLEEP_UPDATER_COMMAND_LINE_H
#define NOSLEEP_UPDATER_COMMAND_LINE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wchar.h>

#define UPDATER_MAX_WINDOWS_COMMAND_LINE_CHARS 32767
#define UPDATER_INTERNAL_RELAUNCH_OPTION "--nosleep-internal-update-relaunch"
#define UPDATER_INTERNAL_RELAUNCH_OPTION_W L"--nosleep-internal-update-relaunch"
#define UPDATER_INTERNAL_READY_EVENT_OPTION "--nosleep-internal-update-ready-event"
#define UPDATER_INTERNAL_READY_EVENT_OPTION_W L"--nosleep-internal-update-ready-event"

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

// Append the private readiness-event argument used by the updater relay.
static inline wchar_t* updater_append_ready_event_argument(const wchar_t* command_line,
                                                           const wchar_t* event_name) {
    if (!command_line || !event_name || !*event_name) return NULL;
    for (const wchar_t* cursor = event_name; *cursor; ++cursor) {
        if (!((*cursor >= L'a' && *cursor <= L'z') ||
              (*cursor >= L'A' && *cursor <= L'Z') ||
              (*cursor >= L'0' && *cursor <= L'9') ||
              *cursor == L'\\' || *cursor == L'-')) return NULL;
    }

    size_t length = wcslen(command_line);
    if ((length > 0 && !updater_add_argument_size(&length, 1)) ||
        !updater_add_argument_size(&length, wcslen(UPDATER_INTERNAL_READY_EVENT_OPTION_W)) ||
        !updater_add_argument_size(&length, 1) ||
        !updater_add_argument_size(&length, wcslen(event_name)) ||
        length >= UPDATER_MAX_WINDOWS_COMMAND_LINE_CHARS ||
        length + 1 > (size_t)-1 / sizeof(wchar_t)) return NULL;

    wchar_t* result = (wchar_t*)malloc((length + 1) * sizeof(wchar_t));
    if (!result) return NULL;

    wchar_t* output = result;
    size_t command_line_length = wcslen(command_line);
    if (command_line_length > 0) {
        wmemcpy(output, command_line, command_line_length);
        output += command_line_length;
        *output++ = L' ';
    }
    size_t option_length = wcslen(UPDATER_INTERNAL_READY_EVENT_OPTION_W);
    wmemcpy(output, UPDATER_INTERNAL_READY_EVENT_OPTION_W, option_length);
    output += option_length;
    *output++ = L' ';
    size_t event_name_length = wcslen(event_name);
    wmemcpy(output, event_name, event_name_length);
    output += event_name_length;
    *output = L'\0';
    return result;
}

// Detect the updater's appended private arguments if normal argv parsing fails.
static inline bool updater_command_line_has_ready_event_argument(const wchar_t* command_line) {
    if (!command_line) return false;
    size_t option_length = wcslen(UPDATER_INTERNAL_READY_EVENT_OPTION_W);
    const wchar_t* option = command_line;
    while ((option = wcsstr(option, UPDATER_INTERNAL_READY_EVENT_OPTION_W)) != NULL) {
        bool starts_argument = option == command_line ||
                               option[-1] == L' ' || option[-1] == L'\t';
        const wchar_t* cursor = option + option_length;
        if (starts_argument && (*cursor == L' ' || *cursor == L'\t')) {
            while (*cursor == L' ' || *cursor == L'\t') ++cursor;
            if (*cursor) {
                while (*cursor && *cursor != L' ' && *cursor != L'\t') {
                    if (!((*cursor >= L'a' && *cursor <= L'z') ||
                          (*cursor >= L'A' && *cursor <= L'Z') ||
                          (*cursor >= L'0' && *cursor <= L'9') ||
                          *cursor == L'\\' || *cursor == L'-')) break;
                    ++cursor;
                }
                while (*cursor == L' ' || *cursor == L'\t') ++cursor;
                if (*cursor == L'\0') return true;
            }
        }
        option += option_length;
    }
    return false;
}

#endif
