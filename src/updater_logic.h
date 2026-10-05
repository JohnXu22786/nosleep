// Portable parsing and version comparison used by the updater
#ifndef UPDATER_LOGIC_H
#define UPDATER_LOGIC_H

#include <stdbool.h>

// Struct to hold update information
typedef struct {
    char latest_version[64];   // Latest version string (e.g., "2.0.0")
    char download_url[512];    // Direct download URL for the EXE asset
    char tag_name[64];         // Full tag name (e.g., "v2.0.0")
    bool update_available;     // Whether an update is available
} UpdateInfo;

// Parse a GitHub release response and return true when a usable release is found.
bool updater_parse_response(const char* json_response, UpdateInfo* info);

// Compare two version strings; returns 1, 0 or -1 for greater, equal or less,
// and -2 when either non-NULL version string is malformed.
int updater_compare_versions(const char* v1, const char* v2);

#endif // UPDATER_LOGIC_H
