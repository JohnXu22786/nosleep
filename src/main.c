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

// Structure holding all parsed CLI options (reduces parameter count for parse_arguments)
typedef struct {
    // Run options
    int duration;              // -1 = not set (tray mode default), 0 = indefinite
    int interval;              // seconds
    bool prevent_display;
    bool away_mode;
    bool verbose;
    bool tray_mode;
    bool startup;

    // Track whether legacy CLI flags were explicitly specified
    bool prevent_display_set;
    bool away_mode_set;
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

// Help text shown by --help (static global to avoid stack allocation on each invocation)
static const char* const HELP_TEXT =
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
    "  --add-to-path             Add nosleep directory to environment PATH\n"
    "  --no-add-to-path          Remove nosleep directory from environment PATH\n"
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
    "  nosleep --add-to-path --configure\n"
    "  nosleep --version\n"
    "  nosleep                     (starts tray mode)\n";

// Function prototypes

static int parse_arguments(int argc, wchar_t* argv[], CLIOptions* opts);
static int run_tray_mode(const CLIOptions* opts);
static int run_configure_mode(const CLIOptions* opts);

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
        .prevent_display = false,
        .away_mode = false,
        .verbose = false,
        .tray_mode = false,
        .startup = false,
        .prevent_display_set = false,
        .away_mode_set = false,
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
        // Could not parse command line, default to tray mode
        return run_tray_mode(&opts);
    }
    
    // Parse arguments
    int parse_result = parse_arguments(argc, argv, &opts);
    
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
        printf("%s", HELP_TEXT);
        return 0;
    }
    
    // If --startup was passed and no explicit duration, auto-start indefinitely
    if (opts.startup && opts.duration < 0) {
        opts.duration = 0;
    }
    
    // --version mode: show version and exit
    if (opts.show_version) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        printf("nosleep v" CURRENT_VERSION "\n");
        return 0;
    }
    
    // --configure mode: save settings to registry and exit
    if (opts.configure_mode) {
        // Check if any settings were actually provided
        if (opts.session_finished == CLI_UNSET && opts.auto_start == CLI_UNSET &&
            opts.notification_mode == CLI_UNSET && opts.auto_check_interval == CLI_UNSET &&
            opts.check_updates_startup == CLI_UNSET && opts.add_to_path == CLI_UNSET) {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                freopen("CONOUT$", "w", stdout);
                freopen("CONOUT$", "w", stderr);
            }
            fprintf(stderr, "nosleep: No settings provided with --configure. Use --help for usage.\n");
            return 1;
        }
        return run_configure_mode(&opts);
    }
    
    // Default: run tray mode with CLI overrides
    return run_tray_mode(&opts);
}



static int parse_arguments(int argc, wchar_t* argv[], CLIOptions* opts) {
    for (int i = 1; i < argc; i++) {
        // Convert wide char to UTF-8 for comparison
        char arg[256];
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg, sizeof(arg), NULL, NULL);
        
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            return 2;
        }
        else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0) {
            opts->show_version = true;
        }
        else if (strcmp(arg, "--duration") == 0 || strcmp(arg, "-d") == 0) {
            if (i + 1 >= argc) return 1;
            char value[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, value, sizeof(value), NULL, NULL);
            opts->duration = atoi(value);
        }
        else if (strcmp(arg, "--interval") == 0 || strcmp(arg, "-i") == 0) {
            if (i + 1 >= argc) return 1;
            char value[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, value, sizeof(value), NULL, NULL);
            opts->interval = atoi(value);
            if (opts->interval <= 0) return 1;
        }
        else if (strcmp(arg, "--prevent-display") == 0 || strcmp(arg, "-p") == 0) {
            opts->prevent_display = true;
            opts->prevent_display_set = true;
        }
        else if (strcmp(arg, "--away-mode") == 0 || strcmp(arg, "-a") == 0) {
            opts->away_mode = true;
            opts->away_mode_set = true;
        }
        else if (strcmp(arg, "--verbose") == 0 || strcmp(arg, "-v") == 0) {
            opts->verbose = true;
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
            if (i + 1 >= argc) return 1;
            char value[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, value, sizeof(value), NULL, NULL);
            if (strcmp(value, "none") == 0) {
                opts->session_finished = SESSION_FINISHED_NONE;
            } else if (strcmp(value, "shutdown") == 0) {
                opts->session_finished = SESSION_FINISHED_SHUTDOWN;
            } else if (strcmp(value, "sleep") == 0) {
                opts->session_finished = SESSION_FINISHED_SLEEP;
            } else {
                return 1;
            }
        }
        else if (strcmp(arg, "--notification-mode") == 0) {
            if (i + 1 >= argc) return 1;
            char value[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, value, sizeof(value), NULL, NULL);
            if (strcmp(value, "all") == 0) {
                opts->notification_mode = NOTIFY_ALL;
            } else if (strcmp(value, "critical") == 0) {
                opts->notification_mode = NOTIFY_CRITICAL_ONLY;
            } else if (strcmp(value, "none") == 0) {
                opts->notification_mode = NOTIFY_NONE;
            } else {
                return 1;
            }
        }
        else if (strcmp(arg, "--auto-check-interval") == 0) {
            if (i + 1 >= argc) return 1;
            char value[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, value, sizeof(value), NULL, NULL);
            if (strcmp(value, "never") == 0) {
                opts->auto_check_interval = 0;
            } else if (strcmp(value, "daily") == 0) {
                opts->auto_check_interval = 1;
            } else if (strcmp(value, "weekly") == 0) {
                opts->auto_check_interval = 2;
            } else {
                return 1;
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
            return 1; // Unknown argument
        }
    }
    
    return 0;
}



static int run_configure_mode(const CLIOptions* opts) {
    // Attach to parent console for status output
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    
    bool success = tray_save_settings_cli(opts->session_finished, opts->auto_start,
                           opts->notification_mode, opts->auto_check_interval,
                           opts->check_updates_startup, opts->add_to_path);
    
    if (success) {
        printf("nosleep: Settings saved to registry.\n");
        return 0;
    } else {
        fprintf(stderr, "nosleep: Failed to save settings to registry.\n");
        return 1;
    }
}



static int run_tray_mode(const CLIOptions* opts) {
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
    if (opts->prevent_display_set) {
        tray->prevent_display = opts->prevent_display;
    }
    if (opts->away_mode_set) {
        tray->away_mode = opts->away_mode;
    }
    if (opts->verbose_set) {
        tray->verbose = opts->verbose;
    }
    
    if (opts->session_finished >= 0) {
        tray->session_finished_action = (SessionFinishedAction)opts->session_finished;
    }
    if (opts->auto_start >= 0) {
        tray_set_startup_enabled(tray, opts->auto_start != 0);
    }
    if (opts->notification_mode >= 0) {
        tray->notification_mode = opts->notification_mode;
    }
    if (opts->auto_check_interval >= 0) {
        tray->auto_check_interval = opts->auto_check_interval;
    }
    if (opts->check_updates_startup >= 0) {
        tray->check_updates_on_startup = (opts->check_updates_startup != 0);
    }
    if (opts->add_to_path >= 0) {
        tray_set_add_to_path(tray, opts->add_to_path != 0);
    }
    
    // If duration is specified, auto-start (0 = indefinite, >0 = minutes)
    if (opts->duration >= 0) {
        tray_start_nosleep(tray, opts->duration);
    }
    
    tray_run(tray);
    
    tray_destroy(tray);
    return 0;
}
