// Portable parsing and version comparison used by the updater
#include "updater_logic.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

// Compare two version strings (e.g., "2.0.0" > "1.5.0")
// Returns: 1 if v1 > v2, 0 if equal, -1 if v1 < v2
int updater_compare_versions(const char* v1, const char* v2) {
    if (!v1 || !v2) return 0;

    // Strip leading 'v' or 'V'
    if (v1[0] == 'v' || v1[0] == 'V') v1++;
    if (v2[0] == 'v' || v2[0] == 'V') v2++;

    // Parse major.minor.patch
    int maj1 = 0, min1 = 0, pat1 = 0;
    int maj2 = 0, min2 = 0, pat2 = 0;

    sscanf(v1, "%d.%d.%d", &maj1, &min1, &pat1);
    sscanf(v2, "%d.%d.%d", &maj2, &min2, &pat2);

    if (maj1 > maj2) return 1;
    if (maj1 < maj2) return -1;
    if (min1 > min2) return 1;
    if (min1 < min2) return -1;
    if (pat1 > pat2) return 1;
    if (pat1 < pat2) return -1;
    return 0;
}

// Parse GitHub API response JSON using cJSON
bool updater_parse_response(const char* json_response, UpdateInfo* info) {
    if (!json_response || !info) return false;

    memset(info, 0, sizeof(UpdateInfo));

    cJSON* root = cJSON_Parse(json_response);
    if (!root) return false;

    // Parse "tag_name"
    cJSON* tag_name = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    if (!cJSON_IsString(tag_name) || !tag_name->valuestring) {
        cJSON_Delete(root);
        return false;
    }

    size_t len = strlen(tag_name->valuestring);
    if (len == 0 || len >= sizeof(info->tag_name)) {
        cJSON_Delete(root);
        return false;
    }

    strncpy(info->tag_name, tag_name->valuestring, sizeof(info->tag_name) - 1);
    info->tag_name[sizeof(info->tag_name) - 1] = '\0';

    // Copy to latest_version, stripping leading v/V
    const char* ver = info->tag_name;
    if (ver[0] == 'v' || ver[0] == 'V') ver++;
    strncpy(info->latest_version, ver, sizeof(info->latest_version) - 1);
    info->latest_version[sizeof(info->latest_version) - 1] = '\0';

    // Parse "assets" array for browser_download_url
    // Look for .exe file in the assets
    cJSON* assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    if (cJSON_IsArray(assets)) {
        cJSON* asset = NULL;
        cJSON_ArrayForEach(asset, assets) {
            cJSON* url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
            if (cJSON_IsString(url) && url->valuestring) {
                size_t url_len = strlen(url->valuestring);
                if (url_len > 0 && url_len < sizeof(info->download_url) - 1) {
                    strncpy(info->download_url, url->valuestring, sizeof(info->download_url) - 1);
                    info->download_url[sizeof(info->download_url) - 1] = '\0';

                    // Check if this URL points to an EXE file
                    if (strstr(info->download_url, ".exe") != NULL) {
                        info->update_available = true;
                        cJSON_Delete(root);
                        return true;
                    }
                }
            }
        }
    }

    // If we found tag_name but no EXE asset, still mark as available
    // (maybe the asset URL pattern is different)
    if (info->tag_name[0] != '\0') {
        info->update_available = true;
        // Construct fallback URL
        snprintf(info->download_url, sizeof(info->download_url),
            "https://github.com/JohnXu22786/nosleep/releases/download/%s/nosleep-%s.exe",
            info->tag_name, info->tag_name);
        cJSON_Delete(root);
        return true;
    }

    cJSON_Delete(root);
    return false;
}
