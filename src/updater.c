// Updater implementation for nosleep
#include "updater.h"
#include "constants.h"
#include "resources.h"
#include "updater_batch.h"
#include "updater_command_line.h"
#include "updater_redirect.h"
#include "updater_response_read.h"
#include "updater_url_policy.h"
#include "updater_pe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <shellapi.h>
#include <winhttp.h>
#include <commctrl.h>
#include "updater_stream.h"

// GitHub API endpoint
#define GITHUB_API_HOST L"api.github.com"
#define GITHUB_API_PATH L"/repos/JohnXu22786/nosleep/releases/latest"

typedef struct DownloadTask DownloadTask;

// Forward declarations
static DWORD follow_redirects(HINTERNET hSession, HINTERNET* hRequest, 
                               HINTERNET* hConnect, DWORD timeout_ms,
                               DownloadTask* download_task);
static char* http_get_json(HWND hwnd_parent, const wchar_t* host, const wchar_t* path, 
                            bool* success, const char** error_msg);
static bool download_file(const char* url, const wchar_t* output_path,
                          UpdaterStreamProgress report_progress, void* progress_context,
                          HANDLE cancel_event, DWORD* total_bytes,
                          DownloadTask* download_task);
static bool create_update_batch_script(const char* current_exe_path, 
                                        const wchar_t* downloaded_path,
                                        const char* exe_name,
                                        wchar_t* script_path, size_t script_path_size,
                                        wchar_t* arguments_path, size_t arguments_path_size);
static char* get_exe_name_from_path(const char* path);
static wchar_t* get_temp_path_for(const char* prefix);
static bool updater_pe_read_file_at(void* context, uint64_t offset,
                                    void* buffer, size_t length);
static bool updater_validate_downloaded_executable(const wchar_t* path);

#define WM_UPDATER_DOWNLOAD_PROGRESS (WM_APP + 1)
#define ID_UPDATER_DOWNLOAD_CANCEL 1001

typedef struct {
    HWND hwnd;
    HWND status_text;
    HWND progress_bar;
    HWND cancel_button;
    HANDLE cancel_event;
    HANDLE worker_thread;
    bool cancellation_requested;
    DownloadTask* task;
} DownloadDialog;

struct DownloadTask {
    DownloadDialog* dialog;
    const char* url;
    const wchar_t* output_path;
    DWORD total_bytes;
    PVOID volatile active_request;
    bool succeeded;
};

static LRESULT CALLBACK download_dialog_proc(HWND hwnd, UINT message,
                                              WPARAM wparam, LPARAM lparam);
static bool register_download_dialog_class(void);
static DWORD WINAPI download_worker(LPVOID parameter);
static bool report_download_progress(void* context, size_t bytes_copied);
static bool download_was_canceled(HANDLE cancel_event);
static bool track_download_request(DownloadTask* task, HINTERNET request);
static void close_download_request(DownloadTask* task, HINTERNET request);

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

static bool updater_write_path(const wchar_t* path, const void* contents, size_t length,
                               DWORD creation_disposition) {
    if (!path || (!contents && length > 0) || length > MAXDWORD) return false;

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, creation_disposition,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD bytes_written = 0;
    BOOL write_ok = TRUE;
    if (length > 0) {
        write_ok = WriteFile(file, contents, (DWORD)length, &bytes_written, NULL);
    }
    CloseHandle(file);
    if (!write_ok || bytes_written != (DWORD)length) {
        DeleteFileW(path);
        return false;
    }
    return true;
}

static bool updater_pe_read_file_at(void* context, uint64_t offset,
                                    void* buffer, size_t length) {
    if (!context || !buffer || length > MAXDWORD) return false;

    LARGE_INTEGER file_offset;
    file_offset.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx((HANDLE)context, file_offset, NULL, FILE_BEGIN)) return false;

    DWORD bytes_read = 0;
    return ReadFile((HANDLE)context, buffer, (DWORD)length, &bytes_read, NULL) &&
           bytes_read == (DWORD)length;
}

static bool updater_validate_downloaded_executable(const wchar_t* path) {
    if (!path) return false;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER file_size;
    bool valid = GetFileSizeEx(file, &file_size) && file_size.QuadPart > 0 &&
                 updater_pe_is_executable((uint64_t)file_size.QuadPart,
                                          updater_pe_read_file_at, file);
    CloseHandle(file);
    return valid;
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

typedef struct {
    const char* message;
    const UpdateInfo* info;
} UpdatePrompt;

static INT_PTR CALLBACK update_prompt_proc(HWND hwnd, UINT message,
                                            WPARAM wparam, LPARAM lparam) {
    UpdatePrompt* prompt = (UpdatePrompt*)GetWindowLongPtrA(hwnd, DWLP_USER);
    if (message == WM_INITDIALOG) {
        prompt = (UpdatePrompt*)lparam;
        SetWindowLongPtrA(hwnd, DWLP_USER, (LONG_PTR)prompt);
        SetDlgItemTextA(hwnd, IDC_UPDATE_MESSAGE, prompt->message);
        if (!prompt->info->release_notes_url[0]) {
            EnableWindow(GetDlgItem(hwnd, IDC_UPDATE_NOTES), FALSE);
            SetDlgItemTextA(hwnd, IDC_UPDATE_NOTES, "Notes unavailable");
        }
        return TRUE;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wparam)) {
            case IDC_UPDATE_NOTES:
                if (prompt && prompt->info->release_notes_url[0] &&
                    (INT_PTR)ShellExecuteA(hwnd, "open", prompt->info->release_notes_url,
                                           NULL, NULL, SW_SHOWNORMAL) <= 32) {
                    MessageBoxA(hwnd, "Could not open release notes in your browser.",
                                "Release notes", MB_OK | MB_ICONWARNING);
                }
                return TRUE;
            case IDYES:
            case IDCANCEL:
                EndDialog(hwnd, LOWORD(wparam));
                return TRUE;
        }
    }
    if (message == WM_CLOSE) {
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
    }
    return FALSE;
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
    
    UpdatePrompt prompt = {msg, info};
    INT_PTR result = DialogBoxParamA(GetModuleHandleA(NULL),
        MAKEINTRESOURCEA(IDD_UPDATE_PROMPT), hwnd_parent,
        update_prompt_proc, (LPARAM)&prompt);
    if (result == -1) {
        // Retain the installation choice if the resource dialog cannot be created.
        result = MessageBoxA(hwnd_parent, msg, title,
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1 | MB_TOPMOST);
    }
    
    return (result == IDYES);
}

static bool register_download_dialog_class(void) {
    static const char* class_name = "NoSleepUpdaterDownloadDialog";
    WNDCLASSA window_class = {0};
    window_class.lpfnWndProc = download_dialog_proc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    window_class.lpszClassName = class_name;
    return RegisterClassA(&window_class) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static void request_download_cancel(DownloadDialog* dialog) {
    if (!dialog || dialog->cancellation_requested) return;
    dialog->cancellation_requested = true;
    if (dialog->cancel_event) SetEvent(dialog->cancel_event);
    if (dialog->task) {
        HINTERNET active_request = (HINTERNET)InterlockedExchangePointer(
            &dialog->task->active_request, NULL);
        if (active_request) WinHttpCloseHandle(active_request);
    }
    if (dialog->worker_thread) {
        typedef BOOL (WINAPI *CancelSynchronousIoFunction)(HANDLE);
        union {
            FARPROC address;
            CancelSynchronousIoFunction function;
        } cancel_synchronous_io;
        HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
        cancel_synchronous_io.address = kernel32
            ? GetProcAddress(kernel32, "CancelSynchronousIo") : NULL;
        if (cancel_synchronous_io.function) {
            cancel_synchronous_io.function(dialog->worker_thread);
        }
    }
    if (dialog->status_text) {
        SetWindowTextA(dialog->status_text, "Cancelling download...");
    }
    if (dialog->cancel_button) {
        EnableWindow(dialog->cancel_button, FALSE);
        SetWindowTextA(dialog->cancel_button, "Cancelling...");
    }
}

static LRESULT CALLBACK download_dialog_proc(HWND hwnd, UINT message,
                                              WPARAM wparam, LPARAM lparam) {
    DownloadDialog* dialog = (DownloadDialog*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    if (message == WM_NCCREATE) {
        CREATESTRUCTA* creation = (CREATESTRUCTA*)lparam;
        dialog = (DownloadDialog*)creation->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)dialog);
        return TRUE;
    }

    switch (message) {
        case WM_CREATE:
            dialog->hwnd = hwnd;
            dialog->status_text = CreateWindowExA(0, "STATIC",
                "Connecting to update server...", WS_CHILD | WS_VISIBLE,
                18, 18, 380, 24, hwnd, NULL, GetModuleHandleA(NULL), NULL);
            dialog->progress_bar = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
                WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
                18, 50, 380, 20, hwnd, NULL, GetModuleHandleA(NULL), NULL);
            dialog->cancel_button = CreateWindowExA(0, "BUTTON", "Cancel",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                310, 88, 88, 28, hwnd,
                (HMENU)(INT_PTR)ID_UPDATER_DOWNLOAD_CANCEL,
                GetModuleHandleA(NULL), NULL);
            if (!dialog->status_text || !dialog->progress_bar || !dialog->cancel_button) {
                return -1;
            }
            SendMessageA(dialog->progress_bar, PBM_SETMARQUEE, TRUE, 30);
            return 0;

        case WM_COMMAND:
            if (LOWORD(wparam) == ID_UPDATER_DOWNLOAD_CANCEL &&
                HIWORD(wparam) == BN_CLICKED) {
                request_download_cancel(dialog);
                return 0;
            }
            break;

        case WM_CLOSE:
            request_download_cancel(dialog);
            return 0;

        case WM_UPDATER_DOWNLOAD_PROGRESS: {
            if (dialog->cancellation_requested) return 0;
            DWORD bytes_downloaded = (DWORD)wparam;
            DWORD total_bytes = (DWORD)lparam;
            char message_text[160];
            if (total_bytes > 0) {
                DWORD downloaded_kb = bytes_downloaded / 1024 + (bytes_downloaded % 1024 != 0);
                DWORD total_kb = total_bytes / 1024 + (total_bytes % 1024 != 0);
                DWORD percent = (DWORD)(((ULONGLONG)bytes_downloaded * 100) / total_bytes);
                if (percent > 100) percent = 100;
                SendMessageA(dialog->progress_bar, PBM_SETMARQUEE, FALSE, 0);
                LONG_PTR style = GetWindowLongPtrA(dialog->progress_bar, GWL_STYLE);
                if (style & PBS_MARQUEE) {
                    SetWindowLongPtrA(dialog->progress_bar, GWL_STYLE,
                                      style & ~((LONG_PTR)PBS_MARQUEE));
                    SetWindowPos(dialog->progress_bar, NULL, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
                }
                SendMessageA(dialog->progress_bar, PBM_SETRANGE32, 0, 100);
                SendMessageA(dialog->progress_bar, PBM_SETPOS, percent, 0);
                snprintf(message_text, sizeof(message_text),
                         "Downloading update... %lu%% (%lu of %lu KB)",
                         (unsigned long)percent,
                         (unsigned long)downloaded_kb,
                         (unsigned long)total_kb);
            } else {
                DWORD downloaded_kb = bytes_downloaded / 1024 + (bytes_downloaded % 1024 != 0);
                snprintf(message_text, sizeof(message_text),
                         "Downloading update... %lu KB received",
                         (unsigned long)downloaded_kb);
            }
            SetWindowTextA(dialog->status_text, message_text);
            return 0;
        }

        case WM_NCDESTROY:
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
            break;
    }

    return DefWindowProcA(hwnd, message, wparam, lparam);
}

static bool download_was_canceled(HANDLE cancel_event) {
    return cancel_event && WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0;
}

static bool track_download_request(DownloadTask* task, HINTERNET request) {
    if (!task) return true;
    InterlockedExchangePointer(&task->active_request, request);
    if (task->dialog && download_was_canceled(task->dialog->cancel_event)) {
        close_download_request(task, request);
        return false;
    }
    return true;
}

static void close_download_request(DownloadTask* task, HINTERNET request) {
    if (!request) return;
    if (task && (HINTERNET)InterlockedCompareExchangePointer(
            &task->active_request, NULL, request) != request) {
        return;
    }
    WinHttpCloseHandle(request);
}

static bool report_download_progress(void* context, size_t bytes_copied) {
    DownloadTask* task = (DownloadTask*)context;
    if (!task || !task->dialog || download_was_canceled(task->dialog->cancel_event)) {
        return false;
    }

    DWORD reported_bytes = bytes_copied > MAXDWORD ? MAXDWORD : (DWORD)bytes_copied;
    return PostMessageA(task->dialog->hwnd, WM_UPDATER_DOWNLOAD_PROGRESS,
                        (WPARAM)reported_bytes, (LPARAM)task->total_bytes) != FALSE &&
           !download_was_canceled(task->dialog->cancel_event);
}

static DWORD WINAPI download_worker(LPVOID parameter) {
    DownloadTask* task = (DownloadTask*)parameter;
    task->succeeded = download_file(task->url, task->output_path,
                                    report_download_progress, task,
                                    task->dialog->cancel_event, &task->total_bytes, task);
    return 0;
}

// Batch files are written as UTF-8 and select that code page before reading paths.
static char* updater_path_to_utf8(const wchar_t* path) {
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                                    NULL, 0, NULL, NULL);
    if (length == 0) return NULL;
    char* utf8 = (char*)malloc((size_t)length);
    if (!utf8) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                            utf8, length, NULL, NULL)) {
        free(utf8);
        return NULL;
    }
    return utf8;
}

// Download a new version and perform the update
static bool updater_download_and_install_utf8(UpdateInfo* info, const char* current_exe_path, HWND hwnd_parent) {
    if (!info || !current_exe_path) return false;
    
    // Get temp path for downloaded file
    wchar_t* temp_path = get_temp_path_for("nosleep_update");
    if (!temp_path) return false;
    
    DownloadDialog dialog = {0};
    DownloadTask task = {&dialog, info->download_url, temp_path, 0, NULL, false};
    dialog.task = &task;
    bool download_ok = false;
    bool worker_started = false;
    bool message_loop_quit = false;
    int quit_code = 0;
    INITCOMMONCONTROLSEX common_controls = {0};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_PROGRESS_CLASS;

    dialog.cancel_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (dialog.cancel_event && InitCommonControlsEx(&common_controls) &&
        register_download_dialog_class()) {
        dialog.hwnd = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
            "NoSleepUpdaterDownloadDialog", "Downloading Update",
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, 430, 160,
            hwnd_parent, NULL, GetModuleHandleA(NULL), &dialog);
    }

    if (dialog.hwnd) {
        RECT rc;
        GetWindowRect(dialog.hwnd, &rc);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(dialog.hwnd, HWND_TOPMOST,
            (sw - (rc.right - rc.left)) / 2,
            (sh - (rc.bottom - rc.top)) / 2,
            0, 0, SWP_NOSIZE);
        if (hwnd_parent && IsWindow(hwnd_parent)) EnableWindow(hwnd_parent, FALSE);
        ShowWindow(dialog.hwnd, SW_SHOW);
        UpdateWindow(dialog.hwnd);
        SetForegroundWindow(dialog.hwnd);

        dialog.worker_thread = CreateThread(NULL, 0, download_worker, &task, 0, NULL);
        if (dialog.worker_thread) {
            worker_started = true;
            MSG message;
            for (;;) {
                DWORD wait_result = MsgWaitForMultipleObjects(1, &dialog.worker_thread,
                    FALSE, INFINITE, QS_ALLINPUT);
                if (wait_result == WAIT_OBJECT_0) {
                    // Handle a cancel/close already queued when the worker signaled.
                    HWND download_windows[] = {
                        dialog.hwnd, dialog.status_text,
                        dialog.progress_bar, dialog.cancel_button
                    };
                    for (size_t i = 0; i < sizeof(download_windows) / sizeof(download_windows[0]); i++) {
                        while (PeekMessageA(&message, download_windows[i], 0, 0, PM_REMOVE)) {
                            TranslateMessage(&message);
                            DispatchMessageA(&message);
                        }
                    }
                    while (PeekMessageA(&message, NULL, WM_QUIT, WM_QUIT, PM_REMOVE)) {
                        message_loop_quit = true;
                        quit_code = (int)message.wParam;
                        request_download_cancel(&dialog);
                    }
                    break;
                }
                if (wait_result == WAIT_OBJECT_0 + 1) {
                    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
                        if (message.message == WM_QUIT) {
                            message_loop_quit = true;
                            quit_code = (int)message.wParam;
                            request_download_cancel(&dialog);
                            continue;
                        }
                        TranslateMessage(&message);
                        DispatchMessageA(&message);
                    }
                } else {
                    request_download_cancel(&dialog);
                    break;
                }
            }
            WaitForSingleObject(dialog.worker_thread, INFINITE);
            download_ok = task.succeeded && !dialog.cancellation_requested;
            CloseHandle(dialog.worker_thread);
            dialog.worker_thread = NULL;
        }

        DestroyWindow(dialog.hwnd);
        dialog.hwnd = NULL;
        if (hwnd_parent && IsWindow(hwnd_parent)) {
            EnableWindow(hwnd_parent, TRUE);
            SetForegroundWindow(hwnd_parent);
        }
    }

    if (dialog.cancel_event) {
        CloseHandle(dialog.cancel_event);
        dialog.cancel_event = NULL;
    }
    if (message_loop_quit) PostQuitMessage(quit_code);

    if (!worker_started) {
        // Keep updates available if the progress UI or its worker cannot be created.
        download_ok = download_file(info->download_url, temp_path,
                                    NULL, NULL, NULL, NULL, NULL);
    }
    
    if (!download_ok) {
        if (dialog.cancellation_requested) {
            DeleteFileW(temp_path);
            free(temp_path);
            return false;
        }
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), 
            "Failed to download update from:\n%s\n\nPlease check your internet connection and try again.",
            info->download_url);
        MessageBox(hwnd_parent, err_msg, "Download Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        free(temp_path);
        return false;
    }
    
    // Reject proxy and login pages before preparing the executable replacement.
    if (!updater_validate_downloaded_executable(temp_path)) {
        MessageBox(hwnd_parent, "Downloaded file is not a valid Windows executable.\nPlease try again.",
            "Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        DeleteFileW(temp_path);
        free(temp_path);
        return false;
    }
    
    // Get the EXE name from current path
    char* exe_name = get_exe_name_from_path(current_exe_path);
    if (!exe_name) {
        DeleteFileW(temp_path);
        free(temp_path);
        return false;
    }
    
    // Create update batch script in temp directory
    wchar_t script_path[MAX_PATH];
    wchar_t arguments_path[MAX_PATH];
    if (!create_update_batch_script(current_exe_path, temp_path, exe_name, 
                                    script_path, MAX_PATH,
                                    arguments_path, MAX_PATH)) {
        DeleteFileW(temp_path);
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
    
    // Keep recovery instructions and the failure prompt visible after this app exits.
    SHELLEXECUTEINFOW sei = {0};
    sei.cbSize = sizeof(SHELLEXECUTEINFOW);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpFile = script_path;
    sei.nShow = SW_SHOWNORMAL;
    
    if (!ShellExecuteExW(&sei)) {
        DeleteFileW(script_path);
        DeleteFileW(arguments_path);
        DeleteFileW(temp_path);
        free(exe_name);
        free(temp_path);
        MessageBox(hwnd_parent, "Failed to launch update process.\nPlease try the update again.",
            "Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return false;
    }
    
    free(exe_name);
    free(temp_path);
    
    // Signal the application to exit (caller should handle this)
    return true;
}

bool updater_download_and_install(UpdateInfo* info, const wchar_t* current_exe_path,
                                  HWND hwnd_parent) {
    if (!info || !current_exe_path) return false;
    char* utf8_path = updater_path_to_utf8(current_exe_path);
    if (!utf8_path) return false;
    bool started = updater_download_and_install_utf8(info, utf8_path, hwnd_parent);
    free(utf8_path);
    return started;
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

static wchar_t* get_temp_path_for(const char* prefix) {
    wchar_t temp_dir[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, temp_dir);
    if (len == 0 || len >= MAX_PATH) return NULL;
    
    wchar_t* path = (wchar_t*)malloc(MAX_PATH * sizeof(wchar_t));
    if (!path) return NULL;
    
    // Ensure unique name
    char unique_name[64];
    snprintf(unique_name, sizeof(unique_name), "%s_%lu_%lu.exe", 
             prefix, GetCurrentProcessId(), GetTickCount());
    
    size_t name_length = strlen(unique_name);
    if ((size_t)len + name_length >= MAX_PATH) {
        free(path);
        return NULL;
    }
    wmemcpy(path, temp_dir, len);
    // The generated filename is ASCII; only the directory comes from Windows.
    for (size_t i = 0; i <= name_length; i++) {
        path[len + i] = (wchar_t)(unsigned char)unique_name[i];
    }
    return path;
}

// Follow HTTP redirects (301, 302, 307, 308) up to 5 times
// Updates hRequest and hConnect through pointers; sets them to NULL on error
// Returns the final HTTP status code (0 if error occurred during redirect)
static DWORD follow_redirects(HINTERNET hSession, HINTERNET* hRequest, 
                               HINTERNET* hConnect, DWORD timeout_ms,
                               DownloadTask* download_task) {
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
        
        if (!updater_url_is_https(redirect_url)) {
            close_download_request(download_task, *hRequest);
            *hRequest = NULL;
            WinHttpCloseHandle(*hConnect);
            *hConnect = NULL;
            status_code = 0;
            break;
        }
        
        // Close old handles
        close_download_request(download_task, *hRequest);
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

        if (!WinHttpCrackUrl(redirect_url, 0, 0, &newUrlComp) ||
            newUrlComp.nScheme != INTERNET_SCHEME_HTTPS) {
            *hRequest = NULL;
            *hConnect = NULL;
            status_code = 0;
            break;
        }

        if (!updater_build_request_target(newPath, newExtraInfo,
                                          newRequestTarget,
                                          sizeof(newRequestTarget) / sizeof(newRequestTarget[0]))) {
            *hRequest = NULL;
            *hConnect = NULL;
            break;
        }
        
        // Create new connection
        *hConnect = WinHttpConnect(hSession, newHost, newUrlComp.nPort, 0);
        if (!*hConnect) {
            *hRequest = NULL;
            break;
        }
        
        // Create new request
        DWORD newFlags = WINHTTP_FLAG_REFRESH | WINHTTP_FLAG_SECURE;
        
        *hRequest = WinHttpOpenRequest(*hConnect, L"GET", newRequestTarget,
                                        NULL, NULL, NULL, newFlags);
        if (!*hRequest) {
            WinHttpCloseHandle(*hConnect);
            *hConnect = NULL;
            break;
        }
        if (!track_download_request(download_task, *hRequest)) {
            *hRequest = NULL;
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
            close_download_request(download_task, *hRequest);
            *hRequest = NULL;
            WinHttpCloseHandle(*hConnect);
            *hConnect = NULL;
            break;
        }
        if (!WinHttpReceiveResponse(*hRequest, NULL)) {
            close_download_request(download_task, *hRequest);
            *hRequest = NULL;
            WinHttpCloseHandle(*hConnect);
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
    DWORD status_code = follow_redirects(hSession, &hRequest, &hConnect, timeout, NULL);
    
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
    
    char* response = NULL;
    UpdaterResponseReadResult read_result = updater_read_response_body(
        updater_read_winhttp, hRequest, (uint32_t)content_length, &response);
    if (read_result != UPDATER_RESPONSE_READ_OK) {
        WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (error_msg) {
            switch (read_result) {
                case UPDATER_RESPONSE_READ_TOO_LARGE:
                    *error_msg = "Response is too large";
                    break;
                case UPDATER_RESPONSE_READ_OUT_OF_MEMORY:
                    *error_msg = "Out of memory";
                    break;
                case UPDATER_RESPONSE_READ_OUT_OF_MEMORY_GROWTH:
                    *error_msg = "Out of memory during download";
                    break;
                default:
                    *error_msg = "Could not read response";
                    break;
            }
        }
        return NULL;
    }
    
    WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    *success = true;
    return response;
}

// Download a file from URL to local path
static bool download_file(const char* url, const wchar_t* output_path,
                          UpdaterStreamProgress report_progress, void* progress_context,
                          HANDLE cancel_event, DWORD* total_bytes,
                          DownloadTask* download_task) {
    if (total_bytes) *total_bytes = 0;
    if (!url || !output_path || download_was_canceled(cancel_event)) return false;
    
    // Parse the URL
    // Expected format: https://github.com/.../nosleep-vX.X.X.exe
    wchar_t wurl[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048) == 0 ||
        !updater_url_is_https(wurl)) {
        return false;
    }
    
    // Parse URL to extract host, path, and query data
    URL_COMPONENTS urlComp = {0};
    urlComp.dwStructSize = sizeof(urlComp);
    
    wchar_t hostName[256] = {0};
    wchar_t urlPath[2048] = {0};
    wchar_t urlExtraInfo[2048] = {0};
    wchar_t requestTarget[4096] = {0};
    
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 2048;
    urlComp.lpszExtraInfo = urlExtraInfo;
    urlComp.dwExtraInfoLength = 2048;
    
    if (!WinHttpCrackUrl(wurl, 0, 0, &urlComp) ||
        urlComp.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }

    if (!updater_build_request_target(urlPath, urlExtraInfo, requestTarget,
                                      sizeof(requestTarget) / sizeof(requestTarget[0]))) {
        return false;
    }
    
    HINTERNET hSession = WinHttpOpen(L"nosleep-updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return false;
    
    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    DWORD flags = WINHTTP_FLAG_REFRESH | WINHTTP_FLAG_SECURE;
    
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", requestTarget,
        NULL, NULL, NULL, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    if (!track_download_request(download_task, hRequest)) {
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
        close_download_request(download_task, hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        close_download_request(download_task, hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    // GitHub releases may redirect; follow redirects manually
    DWORD status_code = follow_redirects(hSession, &hRequest, &hConnect, timeout,
                                         download_task);
    
    if (status_code == 0 || status_code != 200) {
        if (hRequest) close_download_request(download_task, hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (download_was_canceled(cancel_event)) {
        close_download_request(download_task, hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Bound temporary disk use even for missing or misleading Content-Length.
    const size_t max_download_bytes = 64u * 1024u * 1024u;
    DWORD content_length = 0;
    DWORD content_length_size = sizeof(content_length);
    if (WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &content_length, &content_length_size, NULL)) {
        if (content_length > max_download_bytes) {
            close_download_request(download_task, hRequest);
            if (hConnect) WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }
        if (total_bytes) *total_bytes = content_length;
    }

    if (download_was_canceled(cancel_event)) {
        close_download_request(download_task, hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    // Open output file
    HANDLE hFile = CreateFileW(output_path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        close_download_request(download_task, hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    // Read data and write to file
    DWORD buffer_size = 65536;
    char* buffer = (char*)malloc(buffer_size);
    if (!buffer) {
        CloseHandle(hFile);
        close_download_request(download_task, hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        DeleteFileW(output_path);
        return false;
    }
    
    bool copy_ok = updater_copy_stream(updater_read_winhttp, hRequest,
                                       updater_write_file, hFile,
                                       buffer, buffer_size, max_download_bytes,
                                       report_progress, progress_context);
    
    free(buffer);
    CloseHandle(hFile);
    close_download_request(download_task, hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    bool canceled = download_was_canceled(cancel_event);
    if (!copy_ok || canceled) DeleteFileW(output_path);
    return copy_ok && !canceled;
}

// Create a batch script that:
// 1. Waits for the parent process to exit
// 2. Replaces the old EXE with the downloaded one (preserving original filename)
// 3. Starts the new EXE
// 4. Deletes itself
static bool create_update_batch_script(const char* current_exe_path, 
                                        const wchar_t* downloaded_path,
                                        const char* exe_name,
                                        wchar_t* script_path, size_t script_path_size,
                                        wchar_t* arguments_path, size_t arguments_path_size) {
    if (!current_exe_path || !downloaded_path || !exe_name || !script_path ||
        script_path_size == 0 || !arguments_path || arguments_path_size == 0) return false;

    // Reserve a unique temp file name for the arguments and matching batch script.
    wchar_t temp_dir[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, temp_dir);
    if (len == 0 || len >= MAX_PATH) return false;

    if (arguments_path_size < MAX_PATH ||
        !GetTempFileNameW(temp_dir, L"nsl", 0, arguments_path)) {
        return false;
    }
    size_t arguments_path_length = wcslen(arguments_path);
    const size_t temp_extension_length = sizeof(L".tmp") / sizeof(wchar_t) - 1;
    if (arguments_path_length < temp_extension_length ||
        script_path_size <= arguments_path_length) {
        DeleteFileW(arguments_path);
        return false;
    }
    wmemcpy(script_path, arguments_path, arguments_path_length + 1);
    wmemcpy(script_path + arguments_path_length - temp_extension_length, L".bat",
            sizeof(L".bat") / sizeof(wchar_t));

    int original_argc = 0;
    wchar_t** original_argv = CommandLineToArgvW(GetCommandLineW(), &original_argc);
    if (!original_argv || original_argc < 1) {
        if (original_argv) LocalFree(original_argv);
        DeleteFileW(arguments_path);
        return false;
    }
    wchar_t* original_arguments = updater_build_windows_command_line(original_argc, original_argv);
    LocalFree(original_argv);
    if (!original_arguments) {
        DeleteFileW(arguments_path);
        return false;
    }

    size_t original_arguments_length = wcslen(original_arguments);
    if (original_arguments_length >= UPDATER_MAX_WINDOWS_COMMAND_LINE_CHARS ||
        original_arguments_length > (size_t)MAXDWORD / sizeof(wchar_t)) {
        free(original_arguments);
        DeleteFileW(arguments_path);
        return false;
    }
    bool arguments_written = updater_write_path(arguments_path, original_arguments,
                                                original_arguments_length * sizeof(wchar_t),
                                                CREATE_ALWAYS);
    free(original_arguments);
    if (!arguments_written) {
        DeleteFileW(arguments_path);
        return false;
    }

    // Build the batch script
    // The script:
    // 1. Waits up to 60 seconds for this nosleep process to exit
    // 2. Stages the download beside the EXE and retains a backup until replacement succeeds
    // 3. Starts the new EXE with the original command-line arguments
    // 4. Deletes itself
    
    char* escaped_current_exe_path = updater_escape_batch_path(current_exe_path);
    char* downloaded_utf8 = updater_path_to_utf8(downloaded_path);
    char* arguments_utf8 = updater_path_to_utf8(arguments_path);
    char* escaped_downloaded_path = downloaded_utf8 ? updater_escape_batch_path(downloaded_utf8) : NULL;
    char* escaped_exe_name = updater_escape_batch_path(exe_name);
    char* escaped_arguments_path = arguments_utf8 ? updater_escape_batch_path(arguments_utf8) : NULL;
    free(downloaded_utf8);
    free(arguments_utf8);
    char* escaped_internal_marker =
        updater_escape_batch_path(UPDATER_INTERNAL_RELAUNCH_OPTION);
    if (!escaped_current_exe_path || !escaped_downloaded_path || !escaped_exe_name ||
        !escaped_arguments_path || !escaped_internal_marker) {
        free(escaped_current_exe_path);
        free(escaped_downloaded_path);
        free(escaped_exe_name);
        free(escaped_arguments_path);
        free(escaped_internal_marker);
        DeleteFileW(arguments_path);
        return false;
    }

    char* downloaded_path_error_line =
        updater_format_batch_error_path_line(escaped_downloaded_path);
    if (!downloaded_path_error_line) {
        free(escaped_current_exe_path);
        free(escaped_downloaded_path);
        free(escaped_exe_name);
        free(escaped_arguments_path);
        free(escaped_internal_marker);
        DeleteFileW(arguments_path);
        return false;
    }

    char script_content[32768];
    int written = snprintf(script_content, sizeof(script_content),
        "@echo off\r\n"
        "chcp 65001 > nul\r\n"
        "setlocal DisableDelayedExpansion\r\n"
        "title Updating nosleep...\r\n"
        "echo Waiting for nosleep to close...\r\n"
        "set /a wait_attempts=0\r\n"
        ":WAITLOOP\r\n"
        "timeout /t 2 /nobreak > nul\r\n"
        "tasklist /FI \"PID eq %lu\" /FO CSV /NH 2>nul | find /I \"%s\" > nul\r\n"
        "if errorlevel 1 goto REPLACE\r\n"
        "set /a wait_attempts=wait_attempts+1\r\n"
        "if %%wait_attempts%% GEQ 30 goto FAILED\r\n"
        "goto WAITLOOP\r\n"
        ":REPLACE\r\n"
        "echo Replacing executable...\r\n"
        "if exist \"%s.update\" goto FAILED\r\n"
        "if exist \"%s.backup\" goto FAILED\r\n"
        "copy /Y \"%s\" \"%s.update\" > nul\r\n"
        "if errorlevel 1 goto CLEAN_FAILED\r\n"
        "move /Y \"%s\" \"%s.backup\" > nul\r\n"
        "if errorlevel 1 goto CLEAN_FAILED\r\n"
        "move /Y \"%s.update\" \"%s\" > nul\r\n"
        "if errorlevel 1 goto ROLLBACK\r\n"
        "del \"%s.backup\" > nul 2>&1\r\n"
        "goto START\r\n"
        ":ROLLBACK\r\n"
        "move /Y \"%s.backup\" \"%s\" > nul\r\n"
        "if errorlevel 1 echo Restore failed. The original executable is retained with a .backup suffix.\r\n"
        ":CLEAN_FAILED\r\n"
        "del \"%s.update\" > nul 2>&1\r\n"
        ":FAILED\r\n"
        "echo Failed to update. Check free space and file permissions.\r\n"
        "echo.\r\n"
        "echo The downloaded file is at:\r\n"
        "%s"
        "echo.\r\n"
        "del \"%s\" > nul 2>&1\r\n"
        "pause\r\n"
        "exit /b 1\r\n"
        ":START\r\n"
        "echo Update complete! Starting nosleep...\r\n"
        "start /wait \"\" \"%s\" %s \"%s\"\r\n"
        "echo Cleaning up...\r\n"
        "del \"%s\" > nul 2>&1\r\n"
        "del \"%s\" > nul 2>&1\r\n"
        "del \"%%~f0\" > nul 2>&1\r\n",
        (unsigned long)GetCurrentProcessId(), // wait only for the updating process
        escaped_exe_name,            // for find
        escaped_current_exe_path,    // reject a pre-existing staging file
        escaped_current_exe_path,    // reject a pre-existing backup
        escaped_downloaded_path,     // source file to stage
        escaped_current_exe_path,    // stage on the destination volume
        escaped_current_exe_path,    // preserve the installed executable
        escaped_current_exe_path,    // backup on the same volume
        escaped_current_exe_path,    // staged executable to install
        escaped_current_exe_path,    // destination (original EXE path)
        escaped_current_exe_path,    // remove backup after successful replacement
        escaped_current_exe_path,    // restore original on installation failure
        escaped_current_exe_path,    // original EXE path for rollback
        escaped_current_exe_path,    // remove failed staging file
        downloaded_path_error_line,  // info message about temp file
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
    free(downloaded_path_error_line);
    
    if (written <= 0 || (size_t)written >= sizeof(script_content)) {
        DeleteFileW(arguments_path);
        return false;
    }
    
    // Write script file
    if (!updater_write_path(script_path, script_content, strlen(script_content), CREATE_NEW)) {
        DeleteFileW(arguments_path);
        return false;
    }
    
    return true;
}
