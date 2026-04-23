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

// Function prototypes

static int parse_arguments(int argc, wchar_t* argv[], 
                          int* duration, int* interval,
                          bool* prevent_display, bool* away_mode,
                          bool* verbose, bool* tray_mode);
static int run_tray_mode(bool prevent_display, bool away_mode, bool verbose, int duration_minutes);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
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
    
    // Parse command line arguments using Windows API
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    
    if (!argv) {
        // Could not parse command line, default to tray mode
        return run_tray_mode(prevent_display, away_mode, verbose, duration);
    }
    
    // Parse arguments
    int parse_result = parse_arguments(argc, argv, &duration, &interval,
                                      &prevent_display, &away_mode,
                                      &verbose, &tray_mode);
    
    LocalFree(argv);
    
    if (parse_result == 1) {
        // Error parsing arguments - output to console
        // Try to attach to parent console if available
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        fprintf(stderr, "Invalid command line arguments. Use --help for usage information.\n");
        return 1;
    } else if (parse_result == 2) {
        // Help requested - output to console
        // Try to attach to parent console if available
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        const char* help_text = 
            "nosleep - Prevent Windows from sleeping using SetThreadExecutionState API\n\n"
            "Usage: nosleep [OPTIONS]\n\n"
            "Options:\n"
            "  -d, --duration MINUTES    Duration in minutes to prevent sleep\n"
            "                            (positive integer, 0 or negative = indefinite)\n"
            "  -i, --interval SECONDS    Interval in seconds to refresh sleep prevention\n"
            "                            (default: 20)\n"
            "  -p, --prevent-display     Also prevent display from sleeping\n"
            "  -a, --away-mode           Enable away mode (requires compatible hardware)\n"
            "  -v, --verbose             Print detailed status to debug output\n"
            "  -t, --tray                Start in system tray mode\n"
            "                            (default if no arguments provided)\n"
            "\n"
            "Examples:\n"
            "  nosleep --duration 30 --prevent-display\n"
            "  nosleep -d 60 -i 10 -v\n"
            "  nosleep --tray\n"
            "  nosleep                     (starts tray mode)\n";
        
        printf("%s", help_text);
        return 0;
    }
    
    // Always run tray mode for pure GUI application
    return run_tray_mode(prevent_display, away_mode, verbose, duration);
}



static int parse_arguments(int argc, wchar_t* argv[], 
                          int* duration, int* interval,
                          bool* prevent_display, bool* away_mode,
                          bool* verbose, bool* tray_mode) {
    // Simple argument parsing (could use getopt or similar, but keep simple for Windows)
    for (int i = 1; i < argc; i++) {
        // Convert wide char to UTF-8 for comparison
        char arg[256];
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg, sizeof(arg), NULL, NULL);
        
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            return 2; // Special code: help requested
        }
        else if (strcmp(arg, "--duration") == 0 || strcmp(arg, "-d") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            // Convert next argument to integer
            char value[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, value, sizeof(value), NULL, NULL);
            *duration = atoi(value);
        }
        else if (strcmp(arg, "--interval") == 0 || strcmp(arg, "-i") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            char value[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, value, sizeof(value), NULL, NULL);
            *interval = atoi(value);
            if (*interval <= 0) {
                return 1;
            }
        }
        else if (strcmp(arg, "--prevent-display") == 0 || strcmp(arg, "-p") == 0) {
            *prevent_display = true;
        }
        else if (strcmp(arg, "--away-mode") == 0 || strcmp(arg, "-a") == 0) {
            *away_mode = true;
        }
        else if (strcmp(arg, "--verbose") == 0 || strcmp(arg, "-v") == 0) {
            *verbose = true;
        }
        else if (strcmp(arg, "--tray") == 0 || strcmp(arg, "-t") == 0) {
            *tray_mode = true;
        }
        else {
            return 1; // Unknown argument
        }
    }
    
    return 0;
}



static int run_tray_mode(bool prevent_display, bool away_mode, bool verbose, int duration_minutes) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        // In debug mode, we can output to debugger
        OutputDebugString("[nosleep] run_tray_mode: starting with debug enabled\n");
        OutputDebugString("Starting nosleep in system tray mode...\n");
        OutputDebugString("Right-click the tray icon to set duration and control nosleep.\n");
    }
    
    NoSleepTray* tray = tray_create();
    if (!tray) {
        // No console for error output, but we can show a message box
        MessageBox(NULL, "Failed to create tray instance", "nosleep - Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    tray->prevent_display = prevent_display;
    tray->away_mode = away_mode;
    tray->verbose = verbose;
    
    if (!tray_init(tray)) {
        MessageBox(NULL, "Failed to initialize tray", "nosleep - Error", MB_OK | MB_ICONERROR);
        tray_destroy(tray);
        return 1;
    }
    
    // If duration is specified, auto-start (0 = indefinite, >0 = minutes)
    if (duration_minutes >= 0) {
        tray_start_nosleep(tray, duration_minutes);
    }
    
    tray_run(tray);
    
    tray_destroy(tray);
    return 0;
}