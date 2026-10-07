// Portable parsing and version comparison used by the updater
#ifndef UPDATER_LOGIC_H
#define UPDATER_LOGIC_H

#include <stdbool.h>

// Struct to hold update information
typedef struct {
    char latest_version[64];   // Latest version string (e.g., "2.0.0")
    char download_url[512];    // Direct download URL for the EXE asset
    char release_notes_url[512]; // Validated GitHub release page, empty if unavailable
    char tag_name[64];         // Full tag name (e.g., "v2.0.0")
    bool update_available;     // Whether a usable Windows installer is available
} UpdateInfo;

// Parse a valid GitHub release response, even when it has no usable Windows installer.
bool updater_parse_response(const char* json_response, UpdateInfo* info);

// Return the validated release page, or the repository's official releases listing.
const char* updater_get_release_page_url(const UpdateInfo* info);

// Compare two version strings; returns 1, 0 or -1 for greater, equal or less,
// and -2 when either non-NULL version string is malformed.
int updater_compare_versions(const char* v1, const char* v2);

#endif // UPDATER_LOGIC_H
