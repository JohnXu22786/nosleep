// Updater implementation for nosleep
#include "updater.h"
#include "constants.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winhttp.h>
#include "cJSON.h"

// GitHub API endpoint
#define GITHUB_API_HOST L"api.github.com"
#define GITHUB_API_PATH L"/repos/JohnXu22786/nosleep/releases/latest"

// Forward declarations
static DWORD follow_redirects(HINTERNET hSession, HINTERNET* hRequest, 
                               HINTERNET* hConnect, DWORD timeout_ms);
static char* http_get_json(HWND hwnd_parent, const wchar_t* host, const wchar_t* path, 
                            bool* success, const char** error_msg);
static bool download_file(const char* url, const char* output_path);
static bool create_update_batch_script(const char* current_exe_path, 
                                        const char* downloaded_path,
                                        const char* exe_name,
                                        char* script_path, size_t script_path_size);
static char* get_exe_name_from_path(const char* path);
static char* get_temp_path_for(const char* prefix);

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

// Check for updates against GitHub releases
bool updater_check(UpdateInfo* info, HWND hwnd_parent) {
    if (!info) return false;
    
    memset(info, 0, sizeof(UpdateInfo));
    (void)hwnd_parent; // Unused in current implementation
    
    bool success = false;
    const char* error_detail = NULL;
    
    char* response = http_get_json(hwnd_parent, GITHUB_API_HOST, GITHUB_API_PATH, 
                                    &success, &error_detail);
    if (!success || !response) {
        if (response) free(response);
        return false;
    }
    
    bool parsed = updater_parse_response(response, info);
    free(response);
    
    return parsed;
}

// Show the "Update Available" dialog
bool updater_show_prompt_dialog(HWND hwnd_parent, UpdateInfo* info) {
    if (!info || !info->update_available) return false;
    
    char msg[512];
    snprintf(msg, sizeof(msg), 
        "Version %s is now available (you have v" CURRENT_VERSION ").\n\n"
        "Do you want to download and install the update?\n\n"
        "The application will close automatically during the update.",
        info->latest_version);
    
    char title[128];
    snprintf(title, sizeof(title), "Update Available - nosleep");
    
    int result = MessageBox(hwnd_parent, msg, title, 
                             MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1 | MB_TOPMOST);
    
    return (result == IDYES);
}

// Download a new version and perform the update
bool updater_download_and_install(UpdateInfo* info, const char* current_exe_path, HWND hwnd_parent) {
    if (!info || !current_exe_path) return false;
    
    // Get temp path for downloaded file
    char* temp_path = get_temp_path_for("nosleep_update");
    if (!temp_path) return false;
    
    // Show downloading message
    char download_msg[256];
    snprintf(download_msg, sizeof(download_msg), 
        "Downloading nosleep %s...\nPlease wait.", info->latest_version);
    
    // Create a simple status dialog
    HWND hStatus = CreateWindowEx(0, "STATIC", download_msg,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 350, 100,
        hwnd_parent, NULL, GetModuleHandle(NULL), NULL);
    
    HFONT hFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    if (hFont) {
        SendMessage(hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    
    if (hStatus) {
        // Center on screen
        RECT rc;
        GetWindowRect(hStatus, &rc);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(hStatus, NULL, 
            (sw - (rc.right - rc.left)) / 2,
            (sh - (rc.bottom - rc.top)) / 2,
            0, 0, SWP_NOSIZE | SWP_NOZORDER);
        ShowWindow(hStatus, SW_SHOW);
        UpdateWindow(hStatus);
    }
    
    // Download the file
    bool download_ok = download_file(info->download_url, temp_path);
    
    if (hStatus) {
        DestroyWindow(hStatus);
    }
    if (hFont) {
        DeleteObject(hFont);
    }
    
    if (!download_ok) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), 
            "Failed to download update from:\n%s\n\nPlease check your internet connection and try again.",
            info->download_url);
        MessageBox(hwnd_parent, err_msg, "Download Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        free(temp_path);
        return false;
    }
    
    // Verify the downloaded file exists and has size > 0
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesEx(temp_path, GetFileExInfoStandard, &fad) || 
        fad.nFileSizeLow == 0) {
        MessageBox(hwnd_parent, "Downloaded file appears to be invalid (0 bytes).\nPlease try again.",
            "Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        DeleteFile(temp_path);
        free(temp_path);
        return false;
    }
    
    // Get the EXE name from current path
    char* exe_name = get_exe_name_from_path(current_exe_path);
    if (!exe_name) {
        free(temp_path);
        return false;
    }
    
    // Create update batch script in temp directory
    char script_path[MAX_PATH];
    if (!create_update_batch_script(current_exe_path, temp_path, exe_name, 
                                    script_path, sizeof(script_path))) {
        free(exe_name);
        free(temp_path);
        MessageBox(hwnd_parent, "Failed to create update script.", 
            "Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return false;
    }
    
    // Show notification that update is about to begin
    char notify_msg[256];
    snprintf(notify_msg, sizeof(notify_msg), 
        "Update downloaded. The application will now close to complete the update.");
    MessageBox(hwnd_parent, notify_msg, "Update Ready", 
        MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    
    // Launch the update batch script
    SHELLEXECUTEINFO sei = {0};
    sei.cbSize = sizeof(SHELLEXECUTEINFO);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpFile = script_path;
    sei.nShow = SW_HIDE;
    
    if (!ShellExecuteEx(&sei)) {
        free(exe_name);
        free(temp_path);
        MessageBox(hwnd_parent, "Failed to launch update process.\nPlease run the update manually.",
            "Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return false;
    }
    
    free(exe_name);
    free(temp_path);
    
    // Signal the application to exit (caller should handle this)
    return true;
}

// --- Helper functions ---

static char* get_exe_name_from_path(const char* path) {
    if (!path) return NULL;
    
    const char* last_backslash = strrchr(path, '\\');
    if (last_backslash) {
        return _strdup(last_backslash + 1);
    }
    return _strdup(path);
}

static char* get_temp_path_for(const char* prefix) {
    char temp_dir[MAX_PATH];
    DWORD len = GetTempPath(MAX_PATH, temp_dir);
    if (len == 0 || len >= MAX_PATH) return NULL;
    
    char* path = (char*)malloc(MAX_PATH);
    if (!path) return NULL;
    
    // Ensure unique name
    char unique_name[64];
    snprintf(unique_name, sizeof(unique_name), "%s_%lu_%lu.exe", 
             prefix, GetCurrentProcessId(), GetTickCount());
    
    snprintf(path, MAX_PATH, "%s%s", temp_dir, unique_name);
    return path;
}

// Follow HTTP redirects (301, 302, 307, 308) up to 5 times
// Updates hRequest and hConnect through pointers; sets them to NULL on error
// Returns the final HTTP status code (0 if error occurred during redirect)
static DWORD follow_redirects(HINTERNET hSession, HINTERNET* hRequest, 
                               HINTERNET* hConnect, DWORD timeout_ms) {
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    
    if (!hRequest || !*hRequest || !hConnect || !*hConnect) return 0;
    
    // Get initial status code
    WinHttpQueryHeaders(*hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        NULL, &status_code, &status_size, NULL);
    
    int redirect_count = 0;
    while ((status_code == 301 || status_code == 302 || 
            status_code == 307 || status_code == 308) && 
           redirect_count < 5) {
        
        // Get redirect URL
        wchar_t redirect_url[2048] = {0};
        DWORD url_size = sizeof(redirect_url);
        WinHttpQueryHeaders(*hRequest, WINHTTP_QUERY_LOCATION,
            NULL, redirect_url, &url_size, NULL);
        
        if (redirect_url[0] == L'\0') {
            *hRequest = NULL;
            *hConnect = NULL;
            break;
        }
        
        // Close old handles
        WinHttpCloseHandle(*hRequest);
        *hRequest = NULL;
        WinHttpCloseHandle(*hConnect);
        *hConnect = NULL;
        
        // Parse new URL
        URL_COMPONENTS newUrlComp = {0};
        newUrlComp.dwStructSize = sizeof(newUrlComp);
        wchar_t newHost[256] = {0};
        wchar_t newPath[2048] = {0};
        newUrlComp.lpszHostName = newHost;
        newUrlComp.dwHostNameLength = 256;
        newUrlComp.lpszUrlPath = newPath;
        newUrlComp.dwUrlPathLength = 2048;
        
        if (!WinHttpCrackUrl(redirect_url, 0, 0, &newUrlComp)) {
            *hRequest = NULL;
            *hConnect = NULL;
            break;
        }
        
        // Create new connection
        *hConnect = WinHttpConnect(hSession, newHost,
            newUrlComp.nScheme == INTERNET_SCHEME_HTTPS ? 
                INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
        if (!*hConnect) {
            *hRequest = NULL;
            break;
        }
        
        // Create new request
        DWORD newFlags = WINHTTP_FLAG_REFRESH;
        if (newUrlComp.nScheme == INTERNET_SCHEME_HTTPS) {
            newFlags |= WINHTTP_FLAG_SECURE;
        }
        
        *hRequest = WinHttpOpenRequest(*hConnect, L"GET", newPath, 
                                        NULL, NULL, NULL, newFlags);
        if (!*hRequest) {
            WinHttpCloseHandle(*hConnect);
            *hConnect = NULL;
            break;
        }
        
        // Set timeouts
        WinHttpSetOption(*hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, 
                         &timeout_ms, sizeof(timeout_ms));
        WinHttpSetOption(*hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, 
                         &timeout_ms, sizeof(timeout_ms));
        
        // Send and receive
        if (!WinHttpSendRequest(*hRequest, NULL, 0, NULL, 0, 0, 0)) {
            *hRequest = NULL;
            *hConnect = NULL;
            break;
        }
        if (!WinHttpReceiveResponse(*hRequest, NULL)) {
            *hRequest = NULL;
            *hConnect = NULL;
            break;
        }
        
        // Query new status code
        status_size = sizeof(status_code);
        WinHttpQueryHeaders(*hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &status_code, &status_size, NULL);
        
        redirect_count++;
    }
    
    return status_code;
}

// Fetch JSON from GitHub API via HTTPS
static char* http_get_json(HWND hwnd_parent, const wchar_t* host, const wchar_t* path, 
                            bool* success, const char** error_msg) {
    *success = false;
    if (error_msg) *error_msg = NULL;
    (void)hwnd_parent;
    
    HINTERNET hSession = WinHttpOpen(L"nosleep-updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        if (error_msg) *error_msg = "Could not initialize network";
        return NULL;
    }
    
    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        if (error_msg) *error_msg = "Could not connect to server";
        return NULL;
    }
    
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, 
        NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (error_msg) *error_msg = "Could not create request";
        return NULL;
    }
    
    // Set timeout to 10 seconds
    DWORD timeout = 10000;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    
    BOOL sent = WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0);
    if (!sent) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (error_msg) *error_msg = "Could not send request";
        return NULL;
    }
    
    BOOL received = WinHttpReceiveResponse(hRequest, NULL);
    if (!received) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (error_msg) *error_msg = "Could not receive response";
        return NULL;
    }
    
    // Follow redirects (GitHub API may redirect)
    DWORD status_code = follow_redirects(hSession, &hRequest, &hConnect, timeout);
    
    if (status_code == 0 || status_code != 200) {
        if (hRequest) WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (error_msg) {
            if (status_code == 0) {
                *error_msg = "Could not follow redirect";
            } else {
                *error_msg = "Server returned non-200 status";
            }
        }
        return NULL;
    }
    
    DWORD content_length = 0;
    DWORD cl_size = sizeof(content_length);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
        NULL, &content_length, &cl_size, NULL);
    
    DWORD total_read = 0;
    DWORD buffer_size = 4096;
    if (content_length > 0) {
        buffer_size = content_length + 1;
    } else {
        buffer_size = 8192;
    }
    
    char* response = (char*)malloc(buffer_size);
    if (!response) {
        WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (error_msg) *error_msg = "Out of memory";
        return NULL;
    }
    memset(response, 0, buffer_size);
    
    DWORD bytes_read = 0;
    while (WinHttpReadData(hRequest, response + total_read, 
                           buffer_size - total_read - 1, &bytes_read) && bytes_read > 0) {
        total_read += bytes_read;
        if (total_read >= buffer_size - 1) {
            buffer_size *= 2;
            char* new_response = (char*)realloc(response, buffer_size);
            if (!new_response) {
                // realloc failed - free original buffer and abort
                free(response);
                WinHttpCloseHandle(hRequest);
                if (hConnect) WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                if (error_msg) *error_msg = "Out of memory during download";
                return NULL;
            }
            response = new_response;
            memset(response + total_read, 0, buffer_size - total_read);
        }
    }
    response[total_read] = '\0';
    
    WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    *success = true;
    return response;
}

// Download a file from URL to local path
static bool download_file(const char* url, const char* output_path) {
    if (!url || !output_path) return false;
    
    // Parse the URL
    // Expected format: https://github.com/.../nosleep-vX.X.X.exe
    wchar_t wurl[2048];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048);
    
    // Parse URL to extract host, path
    URL_COMPONENTS urlComp = {0};
    urlComp.dwStructSize = sizeof(urlComp);
    
    wchar_t hostName[256] = {0};
    wchar_t urlPath[2048] = {0};
    
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 2048;
    
    if (!WinHttpCrackUrl(wurl, 0, 0, &urlComp)) {
        return false;
    }
    
    HINTERNET hSession = WinHttpOpen(L"nosleep-updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return false;
    
    HINTERNET hConnect = WinHttpConnect(hSession, hostName, 
        urlComp.nScheme == INTERNET_SCHEME_HTTPS ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (urlComp.nScheme == INTERNET_SCHEME_HTTPS) {
        flags |= WINHTTP_FLAG_SECURE;
    }
    
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath,
        NULL, NULL, NULL, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    // Set timeout
    DWORD timeout = 30000; // 30 seconds for download
    WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    
    // Send request
    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    // GitHub releases may redirect; follow redirects manually
    DWORD status_code = follow_redirects(hSession, &hRequest, &hConnect, timeout);
    
    if (status_code == 0 || status_code != 200) {
        if (hRequest) WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    // Open output file
    HANDLE hFile = CreateFile(output_path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    // Read data and write to file
    DWORD buffer_size = 65536;
    char* buffer = (char*)malloc(buffer_size);
    if (!buffer) {
        CloseHandle(hFile);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    DWORD bytes_read = 0;
    DWORD total_bytes = 0;
    bool write_error = false;
    while (WinHttpReadData(hRequest, buffer, buffer_size, &bytes_read) && bytes_read > 0) {
        DWORD bytes_written = 0;
        if (!WriteFile(hFile, buffer, bytes_read, &bytes_written, NULL) || bytes_written != bytes_read) {
            write_error = true;
            break;
        }
        total_bytes += bytes_written;
    }
    
    free(buffer);
    CloseHandle(hFile);
    WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    // Return true only if we wrote data and had no write errors
    return total_bytes > 0 && !write_error;
}

// Create a batch script that:
// 1. Waits for the parent process to exit
// 2. Replaces the old EXE with the downloaded one (preserving original filename)
// 3. Starts the new EXE
// 4. Deletes itself
static bool create_update_batch_script(const char* current_exe_path, 
                                        const char* downloaded_path,
                                        const char* exe_name,
                                        char* script_path, size_t script_path_size) {
    if (!current_exe_path || !downloaded_path || !exe_name) return false;
    
    // Create script in temp directory
    char temp_dir[MAX_PATH];
    DWORD len = GetTempPath(MAX_PATH, temp_dir);
    if (len == 0 || len >= MAX_PATH) return false;
    
    snprintf(script_path, script_path_size, "%snosleep_update_%lu.bat", 
             temp_dir, GetTickCount());
    
    // Build the batch script
    // The script:
    // 1. Waits for nosleep to exit by polling tasklist
    // 2. Copies the downloaded file over the current EXE (renaming it)
    // 3. Starts the new EXE with the same arguments
    // 4. Deletes itself
    
    char script_content[4096];
    int written = snprintf(script_content, sizeof(script_content),
        "@echo off\r\n"
        "title Updating nosleep...\r\n"
        "echo Waiting for nosleep to close...\r\n"
        ":WAITLOOP\r\n"
        "timeout /t 2 /nobreak > nul\r\n"
        "tasklist /FI \"IMAGENAME eq %s\" 2>nul | find /I \"%s\" > nul\r\n"
        "if errorlevel 1 goto REPLACE\r\n"
        "goto WAITLOOP\r\n"
        ":REPLACE\r\n"
        "echo Replacing executable...\r\n"
        "copy /Y \"%s\" \"%s\" > nul\r\n"
        "if errorlevel 1 (\r\n"
        "    echo Failed to update. The file may be in use.\r\n"
        "    echo.\r\n"
        "    echo The downloaded file is at:\r\n"
        "    echo   %s\r\n"
        "    echo.\r\n"
        "    pause\r\n"
        "    exit /b 1\r\n"
        ")\r\n"
        "echo Update complete! Starting nosleep...\r\n"
        "start \"\" \"%s\"\r\n"
        "echo Cleaning up...\r\n"
        "del \"%s\" > nul 2>&1\r\n"
        "del \"%%~f0\" > nul 2>&1\r\n",
        exe_name,                    // for tasklist filter
        exe_name,                    // for find
        downloaded_path,             // source file to copy from
        current_exe_path,            // destination (original EXE path)
        downloaded_path,             // info message about temp file
        current_exe_path,            // to start the new version
        downloaded_path              // delete temp file
    );
    
    if (written <= 0 || (size_t)written >= sizeof(script_content)) {
        return false;
    }
    
    // Write script file
    HANDLE hFile = CreateFile(script_path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    
    DWORD bytes_written;
    BOOL write_ok = WriteFile(hFile, script_content, (DWORD)strlen(script_content), &bytes_written, NULL);
    CloseHandle(hFile);
    
    if (!write_ok || bytes_written != strlen(script_content)) {
        DeleteFile(script_path);
        return false;
    }
    
    return true;
}
