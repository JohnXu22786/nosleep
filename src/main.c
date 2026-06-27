// nosleep - Prevent Windows from sleeping using SetThreadExecutionState API
// Main application entry point for Windows GUI application
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <stdbool.h>
#include <shellapi.h>

#include "core.h"
#include "tray.h"
#include "constants.h"

// Sentinel values for tri-state CLI options (-1 = not specified)
#define CLI_UNSET -1
#define CLI_DISABLE 0
#define CLI_ENABLE 1

// Function prototypes

static int parse_arguments(int argc, wchar_t* argv[], 
                          int* duration, int* interval,
                          bool* prevent_display, bool* away_mode,
                          bool* verbose, bool* tray_mode,
                          bool* startup,
                          int* session_finished,
                          int* auto_start,
                          int* notification_mode,
                          int* auto_check_interval,
                          int* check_updates_startup,
                          bool* configure_mode,
                          bool* show_version,
                          bool* prevent_display_set,
                          bool* away_mode_set,
                          bool* verbose_set);
static int run_tray_mode(bool prevent_display, bool away_mode, bool verbose,
                         int duration_minutes,
                         int session_finished,
                         int auto_start,
                         int notification_mode,
                         int auto_check_interval,
                         int check_updates_startup,
                         bool prevent_display_set,
                         bool away_mode_set,
                         bool verbose_set);
static int run_configure_mode(int session_finished,
                              int auto_start,
                              int notification_mode,
                              int auto_check_interval,
                              int check_updates_startup);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Enable DPI awareness for proper font rendering on high-DPI displays
    SetProcessDPIAware();
    
    // Suppress unused parameter warnings
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;
    
    // Default values
    int duration = -1;         // -1 = not set (tray mode default), 0 = indefinite
    int interval = 20;         // seconds
    bool prevent_display = false;
    bool away_mode = false;
    bool verbose = false;
    bool tray_mode = false;
    bool startup = false;
    
    // Track whether legacy CLI flags were explicitly specified
    bool prevent_display_set = false;
    bool away_mode_set = false;
    bool verbose_set = false;
    
    // New batch mode settings (-1 = not specified)
    int session_finished = CLI_UNSET;
    int auto_start = CLI_UNSET;
    int notification_mode = CLI_UNSET;
    int auto_check_interval = CLI_UNSET;
    int check_updates_startup = CLI_UNSET;
    bool configure_mode = false;
    bool show_version = false;
    
    // Parse command line arguments using Windows API
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    
    if (!argv) {
        // Could not parse command line, default to tray mode
        return run_tray_mode(prevent_display, away_mode, verbose, duration,
                            session_finished, auto_start,
                            notification_mode, auto_check_interval,
                            check_updates_startup,
                            prevent_display_set, away_mode_set, verbose_set);
    }
    
    // Parse arguments
    int parse_result = parse_arguments(argc, argv, &duration, &interval,
                                       &prevent_display, &away_mode,
                                       &verbose, &tray_mode, &startup,
                                       &session_finished,
                                       &auto_start,
                                       &notification_mode,
                                       &auto_check_interval,
                                       &check_updates_startup,
                                       &configure_mode,
                                       &show_version,
                                       &prevent_display_set,
                                       &away_mode_set,
                                       &verbose_set);
    
    LocalFree(argv);
    
    if (parse_result == 1) {
        // Error parsing arguments - output to console
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        fprintf(stderr, "Invalid command line arguments. Use --help for usage information.\n");
        return 1;
    } else if (parse_result == 2) {
        // Help requested - output to console
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        const char* help_text = 
            "nosleep - Prevent Windows from sleeping using SetThreadExecutionState API\n\n"
            "Usage: nosleep [OPTIONS]\n\n"
            "Run Options (start nosleep with these settings):\n"
            "  -d, --duration MINUTES    Duration in minutes to prevent sleep\n"
            "                            (positive integer, 0 or negative = indefinite)\n"
            "  -i, --interval SECONDS    Interval in seconds to refresh sleep prevention\n"
            "                            (default: 20)\n"
            "  -p, --prevent-display     Also prevent display from sleeping\n"
            "  -a, --away-mode           Enable away mode (requires compatible hardware)\n"
            "  -v, --verbose             Print detailed status to debug output\n"
            "  -t, --tray                Start in system tray mode\n"
            "                            (default if no arguments provided)\n"
            "  -s, --startup             Start sleep prevention immediately (for Windows startup)\n"
            "\n"
            "Configure Options (persistent settings, saved to registry):\n"
            "  --session-finished MODE   Action after timer expires\n"
            "                            (none, shutdown, sleep)\n"
            "  --notification-mode MODE  Notification mode\n"
            "                            (all, critical, none)\n"
            "  --auto-check-interval I   Update check interval\n"
            "                            (never, daily, weekly)\n"
            "  --auto-start              Enable auto-start with Windows\n"
            "  --no-auto-start           Disable auto-start with Windows\n"
            "  --check-updates-startup   Enable check for updates on startup\n"
            "  --no-check-updates-startup Disable check for updates on startup\n"
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
            "  nosleep --session-finished shutdown --configure\n"
            "  nosleep --auto-start --notification-mode critical --configure\n"
            "  nosleep --version\n"
            "  nosleep                     (starts tray mode)\n";
        
        printf("%s", help_text);
        return 0;
    }
    
    // If --startup was passed and no explicit duration, auto-start indefinitely
    if (startup && duration < 0) {
        duration = 0;
    }
    
    // --version mode: show version and exit
    if (show_version) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        printf("nosleep v" CURRENT_VERSION "\n");
        return 0;
    }
    
    // --configure mode: save settings to registry and exit
    if (configure_mode) {
        // Check if any settings were actually provided
        if (session_finished == CLI_UNSET && auto_start == CLI_UNSET &&
            notification_mode == CLI_UNSET && auto_check_interval == CLI_UNSET &&
            check_updates_startup == CLI_UNSET) {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                freopen("CONOUT$", "w", stdout);
                freopen("CONOUT$", "w", stderr);
            }
            fprintf(stderr, "nosleep: No settings provided with --configure. Use --help for usage.\n");
            return 1;
        }
        return run_configure_mode(session_finished, auto_start,
                                  notification_mode, auto_check_interval,
                                  check_updates_startup);
    }
    
    // Default: run tray mode with CLI overrides
    return run_tray_mode(prevent_display, away_mode, verbose, duration,
                         session_finished, auto_start,
                         notification_mode, auto_check_interval,
                         check_updates_startup,
                         prevent_display_set, away_mode_set, verbose_set);
}



static int parse_arguments(int argc, wchar_t* argv[], 
                          int* duration, int* interval,
                          bool* prevent_display, bool* away_mode,
                          bool* verbose, bool* tray_mode,
                          bool* startup,
                          int* session_finished,
                          int* auto_start,
                          int* notification_mode,
                          int* auto_check_interval,
                          int* check_updates_startup,
                          bool* configure_mode,
                          bool* show_version,
                          bool* prevent_display_set,
                          bool* away_mode_set,
                          bool* verbose_set) {
    for (int i = 1; i < argc; i++) {
        // Convert wide char to UTF-8 for comparison
        char arg[256];
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg, sizeof(arg), NULL, NULL);
        
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            return 2;
        }
        else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0) {
            *show_version = true;
        }
        else if (strcmp(arg, "--duration") == 0 || strcmp(arg, "-d") == 0) {
            if (i + 1 >= argc) return 1;
            char value[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, value, sizeof(value), NULL, NULL);
            *duration = atoi(value);
        }
        else if (strcmp(arg, "--interval") == 0 || strcmp(arg, "-i") == 0) {
            if (i + 1 >= argc) return 1;
            char value[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, value, sizeof(value), NULL, NULL);
            *interval = atoi(value);
            if (*interval <= 0) return 1;
        }
        else if (strcmp(arg, "--prevent-display") == 0 || strcmp(arg, "-p") == 0) {
            *prevent_display = true;
            *prevent_display_set = true;
        }
        else if (strcmp(arg, "--away-mode") == 0 || strcmp(arg, "-a") == 0) {
            *away_mode = true;
            *away_mode_set = true;
        }
        else if (strcmp(arg, "--verbose") == 0 || strcmp(arg, "-v") == 0) {
            *verbose = true;
            *verbose_set = true;
        }
        else if (strcmp(arg, "--tray") == 0 || strcmp(arg, "-t") == 0) {
            *tray_mode = true;
        }
        else if (strcmp(arg, "--startup") == 0 || strcmp(arg, "-s") == 0) {
            *startup = true;
        }
        else if (strcmp(arg, "--configure") == 0) {
            *configure_mode = true;
        }
        else if (strcmp(arg, "--session-finished") == 0) {
            if (i + 1 >= argc) return 1;
            char value[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, value, sizeof(value), NULL, NULL);
            if (strcmp(value, "none") == 0) {
                *session_finished = SESSION_FINISHED_NONE;
            } else if (strcmp(value, "shutdown") == 0) {
                *session_finished = SESSION_FINISHED_SHUTDOWN;
            } else if (strcmp(value, "sleep") == 0) {
                *session_finished = SESSION_FINISHED_SLEEP;
            } else {
                return 1;
            }
        }
        else if (strcmp(arg, "--notification-mode") == 0) {
            if (i + 1 >= argc) return 1;
            char value[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, value, sizeof(value), NULL, NULL);
            if (strcmp(value, "all") == 0) {
                *notification_mode = NOTIFY_ALL;
            } else if (strcmp(value, "critical") == 0) {
                *notification_mode = NOTIFY_CRITICAL_ONLY;
            } else if (strcmp(value, "none") == 0) {
                *notification_mode = NOTIFY_NONE;
            } else {
                return 1;
            }
        }
        else if (strcmp(arg, "--auto-check-interval") == 0) {
            if (i + 1 >= argc) return 1;
            char value[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, value, sizeof(value), NULL, NULL);
            if (strcmp(value, "never") == 0) {
                *auto_check_interval = 0;
            } else if (strcmp(value, "daily") == 0) {
                *auto_check_interval = 1;
            } else if (strcmp(value, "weekly") == 0) {
                *auto_check_interval = 2;
            } else {
                return 1;
            }
        }
        else if (strcmp(arg, "--auto-start") == 0) {
            *auto_start = CLI_ENABLE;
        }
        else if (strcmp(arg, "--no-auto-start") == 0) {
            *auto_start = CLI_DISABLE;
        }
        else if (strcmp(arg, "--check-updates-startup") == 0) {
            *check_updates_startup = CLI_ENABLE;
        }
        else if (strcmp(arg, "--no-check-updates-startup") == 0) {
            *check_updates_startup = CLI_DISABLE;
        }
        else {
            return 1; // Unknown argument
        }
    }
    
    return 0;
}



static int run_configure_mode(int session_finished,
                              int auto_start,
                              int notification_mode,
                              int auto_check_interval,
                              int check_updates_startup) {
    // Attach to parent console for status output
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    
    bool success = tray_save_settings_cli(session_finished, auto_start,
                           notification_mode, auto_check_interval,
                           check_updates_startup);
    
    if (success) {
        printf("nosleep: Settings saved to registry.\n");
        return 0;
    } else {
        fprintf(stderr, "nosleep: Failed to save settings to registry.\n");
        return 1;
    }
}



static int run_tray_mode(bool prevent_display, bool away_mode, bool verbose,
                         int duration_minutes,
                         int session_finished,
                         int auto_start,
                         int notification_mode,
                         int auto_check_interval,
                         int check_updates_startup,
                         bool prevent_display_set,
                         bool away_mode_set,
                         bool verbose_set) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        OutputDebugString("[nosleep] run_tray_mode: starting with debug enabled\n");
        OutputDebugString("Starting nosleep in system tray mode...\n");
        OutputDebugString("Right-click the tray icon to set duration and control nosleep.\n");
    }
    
    NoSleepTray* tray = tray_create();
    if (!tray) {
        MessageBox(NULL, "Failed to create tray instance", "nosleep - Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    if (!tray_init(tray)) {
        MessageBox(NULL, "Failed to initialize tray", "nosleep - Error", MB_OK | MB_ICONERROR);
        tray_destroy(tray);
        return 1;
    }
    
    // Apply CLI overrides AFTER tray_load_settings (called inside tray_init)
    // so that command-line flags take precedence over registry defaults.
    // Only override legacy bool fields if the user explicitly specified them,
    // preserving registry-loaded values when no flag is passed.
    if (prevent_display_set) {
        tray->prevent_display = prevent_display;
    }
    if (away_mode_set) {
        tray->away_mode = away_mode;
    }
    if (verbose_set) {
        tray->verbose = verbose;
    }
    
    if (session_finished >= 0) {
        tray->session_finished_action = (SessionFinishedAction)session_finished;
    }
    if (auto_start >= 0) {
        tray_set_startup_enabled(tray, auto_start != 0);
    }
    if (notification_mode >= 0) {
        tray->notification_mode = notification_mode;
    }
    if (auto_check_interval >= 0) {
        tray->auto_check_interval = auto_check_interval;
    }
    if (check_updates_startup >= 0) {
        tray->check_updates_on_startup = (check_updates_startup != 0);
    }
    
    // If duration is specified, auto-start (0 = indefinite, >0 = minutes)
    if (duration_minutes >= 0) {
        tray_start_nosleep(tray, duration_minutes);
    }
    
    tray_run(tray);
    
    tray_destroy(tray);
    return 0;
}
