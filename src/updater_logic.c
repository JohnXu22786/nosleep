// Portable parsing and version comparison used by the updater
#include "updater_logic.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static bool is_valid_url_port(const char* port, const char* end) {
    if (port == end) return false;

    unsigned int value = 0;
    for (; port < end; port++) {
        if (*port < '0' || *port > '9') return false;
        unsigned int digit = (unsigned int)(*port - '0');
        if (value > (65535u - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    return value > 0;
}

static bool is_valid_url_authority(const char* host, const char* path) {
    if (host == path || *host == ':' || memchr(host, '@', (size_t)(path - host))) {
        return false;
    }

    const char* colon = memchr(host, ':', (size_t)(path - host));
    const char* host_end = colon ? colon : path;
    if (host_end == host) return false;
    for (const char* character = host; character < host_end; character++) {
        if (!((*character >= '0' && *character <= '9') ||
              (*character >= 'a' && *character <= 'z') ||
              (*character >= 'A' && *character <= 'Z') ||
              *character == '.' || *character == '-')) {
            return false;
        }
    }

    return !colon || is_valid_url_port(colon + 1, path);
}

static bool is_valid_exe_asset_url(const char* url, size_t url_len) {
    static const char https_scheme[] = "https://";
    const size_t scheme_len = sizeof(https_scheme) - 1;
    if (!url || url_len <= scheme_len) return false;

    for (size_t i = 0; i < scheme_len; i++) {
        char character = url[i];
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
        if (character != https_scheme[i]) return false;
    }

    const char* host = url + scheme_len;
    const char* path = strpbrk(host, "/?#");
    if (!path || *path != '/' || !is_valid_url_authority(host, path)) return false;

    for (size_t i = 0; i < url_len; i++) {
        unsigned char character = (unsigned char)url[i];
        if (character <= 0x20 || character == 0x7f || character == '\\') {
            return false;
        }
    }

    const char* path_end = strpbrk(path, "?#");
    size_t path_len = path_end ? (size_t)(path_end - path) : url_len - (size_t)(path - url);
    return path_len >= 4 && strncmp(path + path_len - 4, ".exe", 4) == 0;
}

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
                if (url_len < sizeof(info->download_url) - 1 &&
                    is_valid_exe_asset_url(url->valuestring, url_len)) {
                    strncpy(info->download_url, url->valuestring, sizeof(info->download_url) - 1);
                    info->download_url[sizeof(info->download_url) - 1] = '\0';
                    info->update_available = true;
                    cJSON_Delete(root);
                    return true;
                }
            }
        }
    }

    cJSON_Delete(root);
    return false;
}
