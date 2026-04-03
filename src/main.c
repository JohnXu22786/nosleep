// nosleep - Prevent Windows from sleeping using SetThreadExecutionState API
// Main application entry point
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <stdbool.h>

#include "core.h"
#include "tray.h"

// Static variable for Ctrl+C handler
static NoSleep* g_current_nosleep = NULL;

// Console control handler for Ctrl+C
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT) {
        printf("\nInterrupted by user\n");
        if (g_current_nosleep) {
            nosleep_stop(g_current_nosleep);
        }
        return TRUE; // Signal handled
    }
    return FALSE; // Let other handlers process
}

// Function prototypes
static void print_usage(const char* program_name);
static int parse_arguments(int argc, char* argv[], 
                          int* duration, int* interval,
                          bool* prevent_display, bool* away_mode,
                          bool* verbose, bool* tray_mode);
static int run_cli_mode(int duration, int interval,
                       bool prevent_display, bool away_mode,
                       bool verbose);
static int run_tray_mode(bool prevent_display, bool away_mode, bool verbose);

int main(int argc, char* argv[]) {
    printf("main entered\n"); fflush(stdout);
    // Default values
    int duration = 0;          // 0 = indefinite
    int interval = 20;         // seconds
    bool prevent_display = false;
    bool away_mode = false;
    bool verbose = false;
    bool tray_mode = false;
    
    // Parse command line arguments
    int parse_result = parse_arguments(argc, argv, &duration, &interval,
                                      &prevent_display, &away_mode,
                                      &verbose, &tray_mode);
    if (parse_result == 1) {
        return 1; // Error
    } else if (parse_result == 2) {
        return 0; // Help printed successfully
    } else if (parse_result != 0) {
        return parse_result;
    }
    
    // If tray flag is set or no arguments provided, run tray mode
    if (tray_mode || argc == 1) {
        return run_tray_mode(prevent_display, away_mode, verbose);
    }
    
    // Otherwise run CLI mode with provided arguments
    return run_cli_mode(duration, interval, prevent_display, away_mode, verbose);
}

static void print_usage(const char* program_name) {
    printf("nosleep - Prevent Windows from sleeping using SetThreadExecutionState API\n\n");
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Options:\n");
    printf("  -d, --duration MINUTES    Duration in minutes to prevent sleep\n");
    printf("                            (positive integer, 0 or negative = indefinite)\n");
    printf("  -i, --interval SECONDS    Interval in seconds to refresh sleep prevention\n");
    printf("                            (default: 20)\n");
    printf("  -p, --prevent-display     Also prevent display from sleeping\n");
    printf("  -a, --away-mode           Enable away mode (requires compatible hardware)\n");
    printf("  -v, --verbose             Print detailed status on every refresh\n");
    printf("  -t, --tray                Start in system tray mode\n");
    printf("                            (default if no arguments provided)\n");
    printf("\nExamples:\n");
    printf("  %s --duration 30 --prevent-display\n", program_name);
    printf("  %s -d 60 -i 10 -v\n", program_name);
    printf("  %s --tray\n", program_name);
    printf("  %s                     (starts tray mode)\n", program_name);
}

static int parse_arguments(int argc, char* argv[], 
                          int* duration, int* interval,
                          bool* prevent_display, bool* away_mode,
                          bool* verbose, bool* tray_mode) {
    // Simple argument parsing (could use getopt or similar, but keep simple for Windows)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 2; // Special code: help printed
        }
        else if (strcmp(argv[i], "--duration") == 0 || strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --duration requires a value\n");
                return 1;
            }
            *duration = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--interval") == 0 || strcmp(argv[i], "-i") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --interval requires a value\n");
                return 1;
            }
            *interval = atoi(argv[++i]);
            if (*interval <= 0) {
                fprintf(stderr, "Error: interval must be positive\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--prevent-display") == 0 || strcmp(argv[i], "-p") == 0) {
            *prevent_display = true;
        }
        else if (strcmp(argv[i], "--away-mode") == 0 || strcmp(argv[i], "-a") == 0) {
            *away_mode = true;
        }
        else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            *verbose = true;
        }
        else if (strcmp(argv[i], "--tray") == 0 || strcmp(argv[i], "-t") == 0) {
            *tray_mode = true;
        }
        else {
            fprintf(stderr, "Error: Unknown argument '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    return 0;
}

static int run_cli_mode(int duration, int interval,
                       bool prevent_display, bool away_mode,
                       bool verbose) {
    printf("run_cli_mode entered\n"); fflush(stdout);
    
    printf("nosleep - Preventing system sleep\n");
    if (duration > 0) {
        printf("Duration: %d minutes\n", duration);
    } else {
        printf("Duration: indefinite\n");
    }
    printf("Refresh interval: %d seconds\n", interval);
    if (prevent_display) {
        printf("Also preventing display sleep\n");
    }
    if (away_mode) {
        printf("Away mode enabled (may require specific hardware)\n");
    }
    printf("Press Ctrl+C to stop and allow sleep\n");
    fflush(stdout);
    
    NoSleep* ns = nosleep_create();
    if (!ns) {
        fprintf(stderr, "Failed to create NoSleep instance\n");
        return 1;
    }
    
    // Register Ctrl+C handler
    g_current_nosleep = ns;
    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
        fprintf(stderr, "Warning: Could not set console control handler\n");
    }
    
    // Convert duration: 0 or negative means indefinite (pass 0 to nosleep_run)
    int duration_to_pass = duration > 0 ? duration : 0;
    
    int result = nosleep_run(ns, duration_to_pass, interval,
                            prevent_display, away_mode, verbose);
    
    // Unregister Ctrl+C handler
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    g_current_nosleep = NULL;
    
    nosleep_destroy(ns);
    return result;
}

static int run_tray_mode(bool prevent_display, bool away_mode, bool verbose) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] run_tray_mode: starting with debug enabled\n");
    }
    printf("Starting nosleep in system tray mode...\n"); fflush(stdout);
    printf("Right-click the tray icon to set duration and control nosleep.\n"); fflush(stdout);
    
    NoSleepTray* tray = tray_create();
    if (!tray) {
        fprintf(stderr, "Failed to create tray instance\n");
        return 1;
    }
    
    tray->prevent_display = prevent_display;
    tray->away_mode = away_mode;
    tray->verbose = verbose;
    
    if (!tray_init(tray)) {
        fprintf(stderr, "Failed to initialize tray\n");
        tray_destroy(tray);
        return 1;
    }
    
    tray_run(tray);
    
    tray_destroy(tray);
    return 0;
}