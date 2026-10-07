// nosleep - Prevent Windows from sleeping using SetThreadExecutionState API
// Main application entry point for Windows GUI application
#include <stdio.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <stdbool.h>
#include <shellapi.h>
#include <wchar.h>

#include "core.h"
#include "tray.h"
#include "constants.h"
#include "cli_duration.h"
#include "cli_interval.h"
#include "updater_command_line.h"

// Sentinel values for tri-state CLI options (-1 = not specified)
#define CLI_UNSET -1
#define CLI_DISABLE 0
#define CLI_ENABLE 1

// Structure holding all parsed CLI options (reduces parameter count for parse_arguments)
typedef struct {
    // Run options
    int duration;              // -1 = not set (tray mode default), 0 = indefinite
    int interval;              // seconds
    int prevent_display; // CLI_UNSET uses the saved preference.
    int away_mode; // CLI_UNSET uses the saved preference.
    bool verbose;
    bool tray_mode;
    bool startup;

    // Track whether the verbose CLI flag was explicitly specified
    bool verbose_set;

    // Batch mode settings (-1 = not specified)
    int session_finished;
    int auto_start;
    int notification_mode;
    int auto_check_interval;
    int check_updates_startup;
    int add_to_path;
    bool configure_mode;
    bool show_version;
} CLIOptions;

typedef enum {
    CLI_PARSE_ERROR_NONE = 0,
    CLI_PARSE_ERROR_UNKNOWN_OPTION,
    CLI_PARSE_ERROR_MISSING_VALUE,
    CLI_PARSE_ERROR_INVALID_VALUE,
    CLI_PARSE_ERROR_INVALID_ENUM,
    CLI_PARSE_ERROR_REQUIRES_CONFIGURE
} CLIParseErrorKind;

typedef struct {
    CLIParseErrorKind kind;
    const wchar_t* option;
    const wchar_t* value;
    const wchar_t* expected;
} CLIParseError;

// Help text shown by --help (static global to avoid stack allocation on each invocation)
static const char* const HELP_TEXT =
    "nosleep - Prevent Windows from sleeping using SetThreadExecutionState API\n\n"
    "Usage: nosleep [OPTIONS]\n\n"
    "Run Options (start nosleep with these settings):\n"
    "  -d, --duration MINUTES    Duration in minutes to prevent sleep\n"
    "                            (positive integer, 0 or negative = indefinite)\n"
    "  -i, --interval SECONDS    Interval in seconds to refresh sleep prevention\n"
    "                            (integer from 1 to 2147483; default: 20)\n"
    "  -p, --prevent-display     Enable display sleep prevention for this run\n"
    "      --no-prevent-display  Disable display sleep prevention for this run\n"
    "  -a, --away-mode           Enable away mode for this run\n"
    "                            (requires compatible hardware)\n"
    "      --no-away-mode        Disable away mode for this run\n"
    "                            (each setting defaults to its saved preference)\n"
    "  -v, --verbose             Enable detailed debug output for this run\n"
    "      --no-verbose          Disable detailed debug output for this run\n"
    "  -t, --tray                Start in system tray mode\n"
    "                            (default if no arguments provided)\n"
    "  -s, --startup             Start sleep prevention immediately (for Windows startup)\n"
    "\n"
    "Configure Options (persistent settings, saved to registry):\n"
    "  --session-finished MODE   Action after timer expires\n"
    "                            (none, shutdown, shutdown-graceful, sleep)\n"
    "  --notification-mode MODE  Notification mode\n"
    "                            (all, critical, none)\n"
    "  --auto-check-interval I   Update check interval\n"
    "                            (never, daily, weekly)\n"
    "  --auto-start              Enable auto-start with Windows (requires --configure)\n"
    "  --no-auto-start           Disable auto-start with Windows (requires --configure)\n"
    "  --check-updates-startup   Enable check for updates on startup\n"
    "  --no-check-updates-startup Disable check for updates on startup\n"
    "  --add-to-path             Add nosleep directory to environment PATH\n"
    "                            (requires --configure)\n"
    "  --no-add-to-path          Remove nosleep directory from environment PATH\n"
    "                            (requires --configure)\n"
    "  --configure               Save settings to registry and exit\n"
    "                            (use with above options to persist them)\n"
    "\n"
    "Information:\n"
    "  --version, -V             Show version information\n"
    "  -h, --help                Show this help message\n"
    "\n"
    "Examples:\n"
    "  nosleep --duration 30 --prevent-display\n"
    "  nosleep -d 60 -i 10 -v\n"
    "  nosleep --tray\n"
    "  nosleep --session-finished shutdown-graceful --configure\n"
    "  shutdown forces applications closed; shutdown-graceful allows save prompts.\n"
    "  nosleep --auto-start --notification-mode critical --configure\n"
    "  nosleep --add-to-path --configure\n"
    "  nosleep --version\n"
    "  nosleep                     (starts tray mode)\n";

// Function prototypes

static int parse_arguments(int argc, wchar_t* argv[], CLIOptions* opts,
                           CLIParseError* error);
static int run_tray_mode(const CLIOptions* opts, HANDLE startup_ready_event);
static int run_configure_mode(const CLIOptions* opts);

// Preserve inherited CRT streams (including redirected files and pipes).
static void prepare_cli_output(void) {
    int stdout_fd = _fileno(stdout);
    int stderr_fd = _fileno(stderr);
    bool has_stdout = stdout_fd >= 0 && _get_osfhandle(stdout_fd) != -1;
    bool has_stderr = stderr_fd >= 0 && _get_osfhandle(stderr_fd) != -1;

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (has_stdout) SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(stdout_fd));
        else freopen("CONOUT$", "w", stdout);
        if (has_stderr) SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(stderr_fd));
        else freopen("CONOUT$", "w", stderr);
    }
}

static int fail_cli_parse(CLIParseError* error, CLIParseErrorKind kind,
                          const wchar_t* option, const wchar_t* value,
                          const wchar_t* expected) {
    if (error) {
        error->kind = kind;
        error->option = option;
        error->value = value;
        error->expected = expected;
    }
    return 1;
}

static wchar_t* format_cli_parse_error(const CLIParseError* error) {
    const wchar_t* option = error && error->option ? error->option : L"";
    const wchar_t* value = error && error->value ? error->value : L"";
    const wchar_t* expected = error && error->expected ? error->expected : L"";
    size_t capacity = 256 + wcslen(option) + wcslen(value) + wcslen(expected);
    wchar_t* message = (wchar_t*)malloc(capacity * sizeof(wchar_t));
    if (!message) return NULL;

    int written = -1;
    if (error) {
        switch (error->kind) {
            case CLI_PARSE_ERROR_UNKNOWN_OPTION:
                written = swprintf(message, capacity,
                                   L"Unknown option \"%ls\". Use --help for usage information.\n",
                                   option);
                break;
            case CLI_PARSE_ERROR_MISSING_VALUE:
                written = swprintf(message, capacity,
                                   L"Option \"%ls\" requires a value. Expected %ls.\n",
                                   option, expected);
                break;
            case CLI_PARSE_ERROR_INVALID_VALUE:
                written = swprintf(message, capacity,
                                   L"Invalid value \"%ls\" for option \"%ls\"; expected %ls.\n",
                                   value, option, expected);
                break;
            case CLI_PARSE_ERROR_INVALID_ENUM:
                written = swprintf(message, capacity,
                                   L"Invalid value \"%ls\" for option \"%ls\"; expected one of: %ls.\n",
                                   value, option, expected);
                break;
            case CLI_PARSE_ERROR_REQUIRES_CONFIGURE:
                written = swprintf(message, capacity,
                                   L"Option \"%ls\" requires --configure. Add --configure to save this setting and exit.\n",
                                   option);
                break;
            default:
                break;
        }
    }
    if (written < 0 || (size_t)written >= capacity) {
        free(message);
        return NULL;
    }
    return message;
}

static bool write_wide_stderr(const wchar_t* message) {
    HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) return false;

    DWORD console_mode;
    if (GetConsoleMode(output, &console_mode)) {
        size_t remaining = wcslen(message);
        const wchar_t* cursor = message;
        while (remaining > 0) {
            DWORD requested = remaining > 0x7fffffff ? 0x7fffffff : (DWORD)remaining;
            DWORD written = 0;
            if (!WriteConsoleW(output, cursor, requested, &written, NULL) || written == 0) {
                return false;
            }
            cursor += written;
            remaining -= written;
        }
        return true;
    }

    int utf8_size = WideCharToMultiByte(CP_UTF8, 0, message, -1, NULL, 0, NULL, NULL);
    if (utf8_size <= 1) return false;
    char* utf8_message = (char*)malloc((size_t)utf8_size);
    if (!utf8_message) return false;
    bool success = false;
    if (WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8_message,
                            utf8_size, NULL, NULL) == utf8_size) {
        DWORD remaining = (DWORD)(utf8_size - 1);
        char* cursor = utf8_message;
        success = true;
        while (remaining > 0) {
            DWORD written = 0;
            if (!WriteFile(output, cursor, remaining, &written, NULL) || written == 0) {
                success = false;
                break;
            }
            cursor += written;
            remaining -= written;
        }
    }
    free(utf8_message);
    return success;
}

static void print_cli_parse_error(const CLIParseError* error) {
    wchar_t* message = format_cli_parse_error(error);
    if (!message || !write_wide_stderr(message)) {
        fprintf(stderr, "Invalid command line arguments. Use --help for usage information.\n");
    }
    free(message);
}

static int relaunch_from_command_line_file(const wchar_t* arguments_path) {
    HANDLE arguments_file = CreateFileW(arguments_path, GENERIC_READ, 0, NULL,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER file_size;
    if (arguments_file == INVALID_HANDLE_VALUE || !GetFileSizeEx(arguments_file, &file_size) ||
        file_size.QuadPart <= 0 ||
        file_size.QuadPart >
            ((LONGLONG)UPDATER_MAX_WINDOWS_COMMAND_LINE_CHARS - 1) * (LONGLONG)sizeof(wchar_t) ||
        file_size.QuadPart % sizeof(wchar_t) != 0) {
        if (arguments_file != INVALID_HANDLE_VALUE) CloseHandle(arguments_file);
        MessageBoxW(NULL, L"The saved command line could not be read. Please start nosleep manually.",
                    L"Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    DWORD file_bytes = (DWORD)file_size.QuadPart;
    wchar_t* command_line = (wchar_t*)malloc((size_t)file_bytes + sizeof(wchar_t));
    if (!command_line) {
        CloseHandle(arguments_file);
        MessageBoxW(NULL, L"The saved command line could not be read. Please start nosleep manually.",
                    L"Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    DWORD bytes_read = 0;
    DWORD total_bytes_read = 0;
    BOOL read_ok = TRUE;
    while (total_bytes_read < file_bytes) {
        read_ok = ReadFile(arguments_file, (BYTE*)command_line + total_bytes_read,
                           file_bytes - total_bytes_read, &bytes_read, NULL);
        if (!read_ok || bytes_read == 0) break;
        total_bytes_read += bytes_read;
    }
    CloseHandle(arguments_file);

    size_t character_count = file_bytes / sizeof(wchar_t);
    if (!read_ok || total_bytes_read != file_bytes) {
        free(command_line);
        MessageBoxW(NULL, L"The saved command line could not be read. Please start nosleep manually.",
                    L"Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    for (size_t i = 0; i < character_count; ++i) {
        if (command_line[i] == L'\0') {
            free(command_line);
            MessageBoxW(NULL, L"The saved command line is invalid. Please start nosleep manually.",
                        L"Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
            return 1;
        }
    }
    command_line[character_count] = L'\0';

    wchar_t executable_path[MAX_PATH];
    DWORD executable_path_length = GetModuleFileNameW(NULL, executable_path,
                                                       (DWORD)(sizeof(executable_path) /
                                                               sizeof(executable_path[0])));
    if (executable_path_length == 0 ||
        executable_path_length >= sizeof(executable_path) / sizeof(executable_path[0])) {
        free(command_line);
        MessageBoxW(NULL, L"The updated executable path could not be read. Please start nosleep manually.",
                    L"Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    wchar_t startup_event_name[64];
    int startup_event_name_length = swprintf(
        startup_event_name, sizeof(startup_event_name) / sizeof(startup_event_name[0]),
        L"Local\\NoSleepUpdaterReady-%lu", (unsigned long)GetCurrentProcessId());
    if (startup_event_name_length < 0 ||
        (size_t)startup_event_name_length >=
            sizeof(startup_event_name) / sizeof(startup_event_name[0])) {
        free(command_line);
        MessageBoxW(NULL, L"The updated application could not be started. Please start nosleep manually.",
                    L"Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    HANDLE startup_ready_event = CreateEventW(NULL, TRUE, FALSE, startup_event_name);
    if (!startup_ready_event || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (startup_ready_event) CloseHandle(startup_ready_event);
        free(command_line);
        MessageBoxW(NULL, L"The updated application startup could not be verified. Please start nosleep manually.",
                    L"Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    wchar_t* startup_command_line = updater_append_ready_event_argument(
        command_line, startup_event_name);
    free(command_line);
    if (!startup_command_line) {
        CloseHandle(startup_ready_event);
        MessageBoxW(NULL, L"The updated application command line is too long. Please start nosleep manually.",
                    L"Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    STARTUPINFOW startup_info = {0};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info = {0};
    BOOL launched = CreateProcessW(executable_path, startup_command_line, NULL, NULL, FALSE, 0,
                                   NULL, NULL, &startup_info, &process_info);
    free(startup_command_line);
    if (!launched) {
        CloseHandle(startup_ready_event);
        MessageBoxW(NULL, L"The updated application could not be started. Please start nosleep manually.",
                    L"Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    HANDLE startup_handles[] = {startup_ready_event, process_info.hProcess};
    DWORD startup_result = WaitForMultipleObjects(2, startup_handles, FALSE, INFINITE);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    CloseHandle(startup_ready_event);
    if (startup_result != WAIT_OBJECT_0) {
        MessageBoxW(NULL, L"The updated application exited before completing startup. Please start nosleep manually.",
                    L"Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Enable DPI awareness for proper font rendering on high-DPI displays
    SetProcessDPIAware();
    
    // Suppress unused parameter warnings
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;
    
    // Default values in CLIOptions struct
    CLIOptions opts = {
        .duration = -1,              // -1 = not set (tray mode default), 0 = indefinite
        .interval = 20,              // seconds
        .prevent_display = CLI_UNSET,
        .away_mode = CLI_UNSET,
        .verbose = false,
        .tray_mode = false,
        .startup = false,
        .verbose_set = false,
        .session_finished = CLI_UNSET,
        .auto_start = CLI_UNSET,
        .notification_mode = CLI_UNSET,
        .auto_check_interval = CLI_UNSET,
        .check_updates_startup = CLI_UNSET,
        .add_to_path = CLI_UNSET,
        .configure_mode = false,
        .show_version = false
    };
    
    // Parse command line arguments using Windows API
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    
    if (!argv) {
        if (updater_command_line_has_ready_event_argument(GetCommandLineW())) {
            MessageBoxW(NULL, L"The application startup could not be verified. Please start nosleep manually.",
                        L"Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
            return 1;
        }
        // Could not parse command line, default to tray mode
        return run_tray_mode(&opts, NULL);
    }

    if (argc == 3 && wcscmp(argv[1], UPDATER_INTERNAL_RELAUNCH_OPTION_W) == 0) {
        int relaunch_result = relaunch_from_command_line_file(argv[2]);
        LocalFree(argv);
        return relaunch_result;
    }

    HANDLE startup_ready_event = NULL;
    if (argc >= 3 &&
        wcscmp(argv[argc - 2], UPDATER_INTERNAL_READY_EVENT_OPTION_W) == 0) {
        startup_ready_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[argc - 1]);
        if (!startup_ready_event) {
            MessageBoxW(NULL, L"The application startup could not be verified. Please start nosleep manually.",
                        L"Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
            LocalFree(argv);
            return 1;
        }
        argc -= 2;
    }
    
    // Parse arguments
    CLIParseError parse_error = {0};
    int parse_result = parse_arguments(argc, argv, &opts, &parse_error);

    if (parse_result == 1) {
        // Error parsing arguments - output to console
        prepare_cli_output();
        print_cli_parse_error(&parse_error);
        LocalFree(argv);
        return 1;
    }

    LocalFree(argv);

    if (parse_result == 2) {
        // Help requested - output to console
        prepare_cli_output();
        printf("%s", HELP_TEXT);
        return 0;
    }
    
    // If --startup was passed and no explicit duration, auto-start indefinitely
    if (opts.startup && opts.duration < 0) {
        opts.duration = 0;
    }
    
    // --version mode: show version and exit
    if (opts.show_version) {
        prepare_cli_output();
        printf("nosleep v" CURRENT_VERSION "\n");
        return 0;
    }
    
    // --configure mode: save settings to registry and exit
    if (opts.configure_mode) {
        // Check if any settings were actually provided
        if (opts.session_finished == CLI_UNSET && opts.auto_start == CLI_UNSET &&
            opts.notification_mode == CLI_UNSET && opts.auto_check_interval == CLI_UNSET &&
            opts.check_updates_startup == CLI_UNSET && opts.add_to_path == CLI_UNSET) {
            prepare_cli_output();
            fprintf(stderr, "nosleep: No settings provided with --configure. Use --help for usage.\n");
            return 1;
        }
        return run_configure_mode(&opts);
    }
    
    // Default: run tray mode with CLI overrides
    return run_tray_mode(&opts, startup_ready_event);
}



static int parse_arguments(int argc, wchar_t* argv[], CLIOptions* opts,
                           CLIParseError* error) {
    if (error) {
        error->kind = CLI_PARSE_ERROR_NONE;
        error->option = NULL;
        error->value = NULL;
        error->expected = NULL;
    }

    for (int i = 1; i < argc; i++) {
        const wchar_t* option = argv[i];
        // Convert wide char to UTF-8 for comparison
        char arg[256];
        if (WideCharToMultiByte(CP_UTF8, 0, option, -1, arg, sizeof(arg), NULL, NULL) == 0) {
            return fail_cli_parse(error, CLI_PARSE_ERROR_UNKNOWN_OPTION, option, NULL, NULL);
        }
        
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            return 2;
        }
        else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0) {
            opts->show_version = true;
        }
        else if (strcmp(arg, "--duration") == 0 || strcmp(arg, "-d") == 0) {
            if (i + 1 >= argc) {
                return fail_cli_parse(error, CLI_PARSE_ERROR_MISSING_VALUE, option, NULL,
                                      L"an integer number of minutes");
            }
            char value[256];
            const wchar_t* value_wide = argv[++i];
            if (WideCharToMultiByte(CP_UTF8, 0, value_wide, -1, value,
                                    sizeof(value), NULL, NULL) == 0 ||
                !cli_parse_duration(value, &opts->duration)) {
                return fail_cli_parse(error, CLI_PARSE_ERROR_INVALID_VALUE, option, value_wide,
                                      L"an integer number of minutes");
            }
            if (opts->duration < 0) {
                opts->duration = 0; // -1 is the unset sentinel; 0 means indefinite.
            }
        }
        else if (strcmp(arg, "--interval") == 0 || strcmp(arg, "-i") == 0) {
            if (i + 1 >= argc) {
                return fail_cli_parse(error, CLI_PARSE_ERROR_MISSING_VALUE, option, NULL,
                                      L"an integer number of seconds from 1 to 2147483");
            }
            char value[256];
            const wchar_t* value_wide = argv[++i];
            if (WideCharToMultiByte(CP_UTF8, 0, value_wide, -1, value,
                                    sizeof(value), NULL, NULL) == 0 ||
                !cli_parse_refresh_interval(value, &opts->interval)) {
                return fail_cli_parse(error, CLI_PARSE_ERROR_INVALID_VALUE, option, value_wide,
                                      L"an integer number of seconds from 1 to 2147483");
            }
        }
        else if (strcmp(arg, "--prevent-display") == 0 || strcmp(arg, "-p") == 0) {
            opts->prevent_display = CLI_ENABLE;
        }
        else if (strcmp(arg, "--no-prevent-display") == 0) {
            opts->prevent_display = CLI_DISABLE;
        }
        else if (strcmp(arg, "--away-mode") == 0 || strcmp(arg, "-a") == 0) {
            opts->away_mode = CLI_ENABLE;
        }
        else if (strcmp(arg, "--no-away-mode") == 0) {
            opts->away_mode = CLI_DISABLE;
        }
        else if (strcmp(arg, "--verbose") == 0 || strcmp(arg, "-v") == 0) {
            opts->verbose = true;
            opts->verbose_set = true;
        }
        else if (strcmp(arg, "--no-verbose") == 0) {
            opts->verbose = false;
            opts->verbose_set = true;
        }
        else if (strcmp(arg, "--tray") == 0 || strcmp(arg, "-t") == 0) {
            opts->tray_mode = true;
        }
        else if (strcmp(arg, "--startup") == 0 || strcmp(arg, "-s") == 0) {
            opts->startup = true;
        }
        else if (strcmp(arg, "--configure") == 0) {
            opts->configure_mode = true;
        }
        else if (strcmp(arg, "--session-finished") == 0) {
            if (i + 1 >= argc) {
                return fail_cli_parse(error, CLI_PARSE_ERROR_MISSING_VALUE, option, NULL,
                                      L"one of: none, shutdown, shutdown-graceful, sleep");
            }
            char value[256];
            const wchar_t* value_wide = argv[++i];
            if (WideCharToMultiByte(CP_UTF8, 0, value_wide, -1, value,
                                    sizeof(value), NULL, NULL) == 0) {
                return fail_cli_parse(error, CLI_PARSE_ERROR_INVALID_ENUM, option, value_wide,
                                      L"none, shutdown, shutdown-graceful, sleep");
            }
            if (strcmp(value, "none") == 0) {
                opts->session_finished = SESSION_FINISHED_NONE;
            } else if (strcmp(value, "shutdown") == 0) {
                opts->session_finished = SESSION_FINISHED_SHUTDOWN;
            } else if (strcmp(value, "shutdown-graceful") == 0) {
                opts->session_finished = SESSION_FINISHED_SHUTDOWN_GRACEFUL;
            } else if (strcmp(value, "sleep") == 0) {
                opts->session_finished = SESSION_FINISHED_SLEEP;
            } else {
                return fail_cli_parse(error, CLI_PARSE_ERROR_INVALID_ENUM, option, value_wide,
                                      L"none, shutdown, shutdown-graceful, sleep");
            }
        }
        else if (strcmp(arg, "--notification-mode") == 0) {
            if (i + 1 >= argc) {
                return fail_cli_parse(error, CLI_PARSE_ERROR_MISSING_VALUE, option, NULL,
                                      L"one of: all, critical, none");
            }
            char value[256];
            const wchar_t* value_wide = argv[++i];
            if (WideCharToMultiByte(CP_UTF8, 0, value_wide, -1, value,
                                    sizeof(value), NULL, NULL) == 0) {
                return fail_cli_parse(error, CLI_PARSE_ERROR_INVALID_ENUM, option, value_wide,
                                      L"all, critical, none");
            }
            if (strcmp(value, "all") == 0) {
                opts->notification_mode = NOTIFY_ALL;
            } else if (strcmp(value, "critical") == 0) {
                opts->notification_mode = NOTIFY_CRITICAL_ONLY;
            } else if (strcmp(value, "none") == 0) {
                opts->notification_mode = NOTIFY_NONE;
            } else {
                return fail_cli_parse(error, CLI_PARSE_ERROR_INVALID_ENUM, option, value_wide,
                                      L"all, critical, none");
            }
        }
        else if (strcmp(arg, "--auto-check-interval") == 0) {
            if (i + 1 >= argc) {
                return fail_cli_parse(error, CLI_PARSE_ERROR_MISSING_VALUE, option, NULL,
                                      L"one of: never, daily, weekly");
            }
            char value[256];
            const wchar_t* value_wide = argv[++i];
            if (WideCharToMultiByte(CP_UTF8, 0, value_wide, -1, value,
                                    sizeof(value), NULL, NULL) == 0) {
                return fail_cli_parse(error, CLI_PARSE_ERROR_INVALID_ENUM, option, value_wide,
                                      L"never, daily, weekly");
            }
            if (strcmp(value, "never") == 0) {
                opts->auto_check_interval = 0;
            } else if (strcmp(value, "daily") == 0) {
                opts->auto_check_interval = 1;
            } else if (strcmp(value, "weekly") == 0) {
                opts->auto_check_interval = 2;
            } else {
                return fail_cli_parse(error, CLI_PARSE_ERROR_INVALID_ENUM, option, value_wide,
                                      L"never, daily, weekly");
            }
        }
        else if (strcmp(arg, "--auto-start") == 0) {
            opts->auto_start = CLI_ENABLE;
        }
        else if (strcmp(arg, "--no-auto-start") == 0) {
            opts->auto_start = CLI_DISABLE;
        }
        else if (strcmp(arg, "--check-updates-startup") == 0) {
            opts->check_updates_startup = CLI_ENABLE;
        }
        else if (strcmp(arg, "--no-check-updates-startup") == 0) {
            opts->check_updates_startup = CLI_DISABLE;
        }
        else if (strcmp(arg, "--add-to-path") == 0) {
            opts->add_to_path = CLI_ENABLE;
        }
        else if (strcmp(arg, "--no-add-to-path") == 0) {
            opts->add_to_path = CLI_DISABLE;
        }
        else {
            return fail_cli_parse(error, CLI_PARSE_ERROR_UNKNOWN_OPTION, option, NULL, NULL);
        }
    }
    if (!opts->configure_mode && !opts->show_version) {
        if (opts->auto_start != CLI_UNSET) {
            return fail_cli_parse(error, CLI_PARSE_ERROR_REQUIRES_CONFIGURE,
                                  opts->auto_start == CLI_ENABLE ? L"--auto-start" : L"--no-auto-start",
                                  NULL, NULL);
        }
        if (opts->add_to_path != CLI_UNSET) {
            return fail_cli_parse(error, CLI_PARSE_ERROR_REQUIRES_CONFIGURE,
                                  opts->add_to_path == CLI_ENABLE ? L"--add-to-path" : L"--no-add-to-path",
                                  NULL, NULL);
        }
    }

    return 0;
}



static int run_configure_mode(const CLIOptions* opts) {
    // Attach to parent console for status output
    prepare_cli_output();
    
    bool success = tray_save_settings_cli(opts->session_finished, opts->auto_start,
                           opts->notification_mode, opts->auto_check_interval,
                           opts->check_updates_startup, opts->add_to_path);
    
    if (success) {
        printf("nosleep: Settings saved to registry.\n");
        return 0;
    } else {
        fprintf(stderr, "nosleep: Failed to save or apply requested settings.\n");
        return 1;
    }
}



static bool signal_updater_startup_ready_event(HANDLE startup_ready_event) {
    if (!startup_ready_event) return true;
    BOOL signaled = SetEvent(startup_ready_event);
    CloseHandle(startup_ready_event);
    return signaled != FALSE;
}

static int run_tray_mode(const CLIOptions* opts, HANDLE startup_ready_event) {
    prepare_cli_output();

    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] run_tray_mode: starting with debug enabled\n");
        fprintf(stderr, "Starting nosleep in system tray mode...\n");
        fprintf(stderr, "Right-click the tray icon to set duration and control nosleep.\n");
        OutputDebugString("[nosleep] run_tray_mode: starting with debug enabled\n");
        OutputDebugString("Starting nosleep in system tray mode...\n");
        OutputDebugString("Right-click the tray icon to set duration and control nosleep.\n");
    }
    
    NoSleepTray* tray = tray_create();
    if (!tray) {
        MessageBox(NULL, "Failed to create tray instance", "nosleep - Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // Supply startup overrides before initialization consumes these settings.
    tray->startup_cli_overrides_set = true;
    tray->notification_mode_cli_override = opts->notification_mode;
    tray->auto_check_interval_cli_override = opts->auto_check_interval;
    tray->check_updates_startup_cli_override = opts->check_updates_startup;

    if (!tray_init(tray)) {
        MessageBox(NULL, "Failed to initialize tray", "nosleep - Error", MB_OK | MB_ICONERROR);
        tray_destroy(tray);
        return 1;
    }

    if (!signal_updater_startup_ready_event(startup_ready_event)) {
        MessageBox(NULL, "Failed to confirm application startup", "nosleep - Error", MB_OK | MB_ICONERROR);
        tray_destroy(tray);
        return 1;
    }
    
    // Apply CLI overrides AFTER tray_load_settings (called inside tray_init).
    // Keep them separate from saved preferences so later settings saves retain
    // the user's choices for the next run.
    if (opts->prevent_display != CLI_UNSET) {
        tray->prevent_display_cli_override = (opts->prevent_display == CLI_ENABLE);
        tray->prevent_display_cli_override_set = true;
    }
    if (opts->away_mode != CLI_UNSET) {
        tray->away_mode_cli_override = (opts->away_mode == CLI_ENABLE);
        tray->away_mode_cli_override_set = true;
    }
    if (opts->verbose_set) {
        tray->verbose_cli_override = opts->verbose;
        tray->verbose_cli_override_set = true;
    }
    tray->refresh_interval_seconds = opts->interval;
    
    if (opts->session_finished >= 0) {
        tray->session_finished_action = (SessionFinishedAction)opts->session_finished;
        tray_update_session_finished_menu(tray);
    }
    // If duration is specified, auto-start (0 = indefinite, >0 = minutes)
    if (opts->duration >= 0) {
        tray_start_nosleep(tray, opts->duration);
    }
    
    tray_run(tray);
    
    tray_destroy(tray);
    return 0;
}
