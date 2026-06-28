// Updater module for nosleep
// Handles checking for updates, downloading, and installing new versions
#ifndef UPDATER_H
#define UPDATER_H

#include <windows.h>
#include <stdbool.h>

// Struct to hold update information
typedef struct {
    char latest_version[64];   // Latest version string (e.g., "2.0.0")
    char download_url[512];    // Direct download URL for the EXE asset
    char tag_name[64];         // Full tag name (e.g., "v2.0.0")
    bool update_available;     // Whether an update is available
} UpdateInfo;

// Check for updates against GitHub releases
// Returns true if check was successful (result in info)
// silent: if true, don't show error notifications
bool updater_check(UpdateInfo* info, HWND hwnd_parent);

// Download a new version and perform the update
// current_exe_path: full path to the current executable
// info: update information from updater_check
// HWND: parent window for any dialogs
// Returns true if update process started successfully
bool updater_download_and_install(UpdateInfo* info, const char* current_exe_path, HWND hwnd_parent);

// Parse the GitHub API response JSON to extract version and download URL
// Returns true if parsing was successful
bool updater_parse_response(const char* json_response, UpdateInfo* info);

// Compare two version strings (e.g., "2.0.0" > "1.5.0")
// Returns: 1 if v1 > v2, 0 if equal, -1 if v1 < v2
int updater_compare_versions(const char* v1, const char* v2);

// Show the "Update Available" dialog and return user's choice
// Returns true if user wants to download
bool updater_show_prompt_dialog(HWND hwnd_parent, UpdateInfo* info);

#endif // UPDATER_H
