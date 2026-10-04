// Updater implementation for nosleep
#include "updater.h"
#include "constants.h"
#include "updater_batch.h"
#include "updater_command_line.h"
#include "updater_redirect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shellapi.h>
#include <winhttp.h>
#include "updater_stream.h"

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
                                        char* script_path, size_t script_path_size,
                                        char* arguments_path, size_t arguments_path_size);
static char* get_exe_name_from_path(const char* path);
static char* get_temp_path_for(const char* prefix);

static bool updater_read_winhttp(void* context, void* buffer, size_t capacity,
                                 size_t* bytes_read) {
    DWORD bytes = 0;
    BOOL success = WinHttpReadData((HINTERNET)context, buffer, (DWORD)capacity, &bytes);
    *bytes_read = bytes;
    return success != FALSE;
}

static bool updater_write_file(void* context, const void* buffer, size_t bytes_to_write,
                               size_t* bytes_written) {
    DWORD bytes = 0;
    BOOL success = WriteFile((HANDLE)context, buffer, (DWORD)bytes_to_write, &bytes, NULL);
    *bytes_written = bytes;
    return success != FALSE;
}

static bool updater_write_path(const char* path, const void* contents, size_t length,
                               DWORD creation_disposition) {
    if (!path || (!contents && length > 0) || length > MAXDWORD) return false;

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, creation_disposition,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD bytes_written = 0;
    BOOL write_ok = TRUE;
    if (length > 0) {
        write_ok = WriteFile(file, contents, (DWORD)length, &bytes_written, NULL);
    }
    CloseHandle(file);
    if (!write_ok || bytes_written != (DWORD)length) {
        DeleteFileA(path);
        return false;
    }
    return true;
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
    char arguments_path[MAX_PATH];
    if (!create_update_batch_script(current_exe_path, temp_path, exe_name, 
                                    script_path, sizeof(script_path),
                                    arguments_path, sizeof(arguments_path))) {
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
        DeleteFileA(script_path);
        DeleteFileA(arguments_path);
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
        wchar_t newExtraInfo[2048] = {0};
        wchar_t newRequestTarget[4096] = {0};
        newUrlComp.lpszHostName = newHost;
        newUrlComp.dwHostNameLength = 256;
        newUrlComp.lpszUrlPath = newPath;
        newUrlComp.dwUrlPathLength = 2048;
        newUrlComp.lpszExtraInfo = newExtraInfo;
        newUrlComp.dwExtraInfoLength = 2048;

        if (!WinHttpCrackUrl(redirect_url, 0, 0, &newUrlComp)) {
            *hRequest = NULL;
            *hConnect = NULL;
            break;
        }

        if (!updater_build_redirect_target(newPath, newExtraInfo,
                                            newRequestTarget,
                                            sizeof(newRequestTarget) / sizeof(newRequestTarget[0]))) {
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
        
        *hRequest = WinHttpOpenRequest(*hConnect, L"GET", newRequestTarget,
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
    
    bool copy_ok = updater_copy_stream(updater_read_winhttp, hRequest,
                                       updater_write_file, hFile,
                                       buffer, buffer_size);
    
    free(buffer);
    CloseHandle(hFile);
    WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    return copy_ok;
}

// Create a batch script that:
// 1. Waits for the parent process to exit
// 2. Replaces the old EXE with the downloaded one (preserving original filename)
// 3. Starts the new EXE
// 4. Deletes itself
static bool create_update_batch_script(const char* current_exe_path, 
                                        const char* downloaded_path,
                                        const char* exe_name,
                                        char* script_path, size_t script_path_size,
                                        char* arguments_path, size_t arguments_path_size) {
    if (!current_exe_path || !downloaded_path || !exe_name || !script_path ||
        script_path_size == 0 || !arguments_path || arguments_path_size == 0) return false;

    // Reserve a unique temp file name for the arguments and matching batch script.
    char temp_dir[MAX_PATH];
    DWORD len = GetTempPath(MAX_PATH, temp_dir);
    if (len == 0 || len >= MAX_PATH) return false;

    if (arguments_path_size < MAX_PATH ||
        !GetTempFileNameA(temp_dir, "nsl", 0, arguments_path)) {
        return false;
    }
    size_t arguments_path_length = strlen(arguments_path);
    const size_t temp_extension_length = sizeof(".tmp") - 1;
    if (arguments_path_length < temp_extension_length ||
        script_path_size <= arguments_path_length) {
        DeleteFileA(arguments_path);
        return false;
    }
    memcpy(script_path, arguments_path, arguments_path_length + 1);
    memcpy(script_path + arguments_path_length - temp_extension_length, ".bat",
           sizeof(".bat"));

    int original_argc = 0;
    wchar_t** original_argv = CommandLineToArgvW(GetCommandLineW(), &original_argc);
    if (!original_argv || original_argc < 1) {
        if (original_argv) LocalFree(original_argv);
        DeleteFileA(arguments_path);
        return false;
    }
    wchar_t* original_arguments = updater_build_windows_command_line(original_argc, original_argv);
    LocalFree(original_argv);
    if (!original_arguments) {
        DeleteFileA(arguments_path);
        return false;
    }

    size_t original_arguments_length = wcslen(original_arguments);
    if (original_arguments_length >= UPDATER_MAX_WINDOWS_COMMAND_LINE_CHARS ||
        original_arguments_length > (size_t)MAXDWORD / sizeof(wchar_t)) {
        free(original_arguments);
        DeleteFileA(arguments_path);
        return false;
    }
    bool arguments_written = updater_write_path(arguments_path, original_arguments,
                                                original_arguments_length * sizeof(wchar_t),
                                                CREATE_ALWAYS);
    free(original_arguments);
    if (!arguments_written) {
        DeleteFileA(arguments_path);
        return false;
    }

    // Build the batch script
    // The script:
    // 1. Waits for nosleep to exit by polling tasklist
    // 2. Copies the downloaded file over the current EXE (renaming it)
    // 3. Starts the new EXE with the original command-line arguments
    // 4. Deletes itself
    
    char* escaped_current_exe_path = updater_escape_batch_path(current_exe_path);
    char* escaped_downloaded_path = updater_escape_batch_path(downloaded_path);
    char* escaped_exe_name = updater_escape_batch_path(exe_name);
    char* escaped_arguments_path = updater_escape_batch_path(arguments_path);
    char* escaped_internal_marker =
        updater_escape_batch_path(UPDATER_INTERNAL_RELAUNCH_OPTION);
    if (!escaped_current_exe_path || !escaped_downloaded_path || !escaped_exe_name ||
        !escaped_arguments_path || !escaped_internal_marker) {
        free(escaped_current_exe_path);
        free(escaped_downloaded_path);
        free(escaped_exe_name);
        free(escaped_arguments_path);
        free(escaped_internal_marker);
        DeleteFileA(arguments_path);
        return false;
    }

    char script_content[8192];
    int written = snprintf(script_content, sizeof(script_content),
        "@echo off\r\n"
        "setlocal DisableDelayedExpansion\r\n"
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
        "    del \"%s\" > nul 2>&1\r\n"
        "    pause\r\n"
        "    exit /b 1\r\n"
        ")\r\n"
        "echo Update complete! Starting nosleep...\r\n"
        "start /wait \"\" \"%s\" %s \"%s\"\r\n"
        "echo Cleaning up...\r\n"
        "del \"%s\" > nul 2>&1\r\n"
        "del \"%s\" > nul 2>&1\r\n"
        "del \"%%~f0\" > nul 2>&1\r\n",
        escaped_exe_name,            // for tasklist filter
        escaped_exe_name,            // for find
        escaped_downloaded_path,     // source file to copy from
        escaped_current_exe_path,    // destination (original EXE path)
        escaped_downloaded_path,     // info message about temp file
        escaped_arguments_path,      // remove original arguments after copy failure
        escaped_current_exe_path,    // executable to start the argument-file relay
        escaped_internal_marker,     // select relay mode in the updated executable
        escaped_arguments_path,      // original command-line arguments
        escaped_arguments_path,      // remove original arguments after the relay exits
        escaped_downloaded_path      // delete downloaded file
    );

    free(escaped_current_exe_path);
    free(escaped_downloaded_path);
    free(escaped_exe_name);
    free(escaped_arguments_path);
    free(escaped_internal_marker);
    
    if (written <= 0 || (size_t)written >= sizeof(script_content)) {
        DeleteFileA(arguments_path);
        return false;
    }
    
    // Write script file
    if (!updater_write_path(script_path, script_content, strlen(script_content), CREATE_NEW)) {
        DeleteFileA(arguments_path);
        return false;
    }
    
    return true;
}
