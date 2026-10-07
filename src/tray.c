// System tray implementation for nosleep
#include "tray.h"
#include "core.h"
#include "constants.h"
#include "resources.h"
#include "notify_groups.h"
#include "updater.h"
#include "tray_stop_guard.h"
#include "tray_indefinite_icon.h"
#include "tray_numbered_icon.h"
#include "tray_countdown_tooltip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <powrprof.h>
#include <winhttp.h>
#include <commctrl.h>
#include <ctype.h>
#include <limits.h>

// Window class name for tray icon
static const char* TRAY_WINDOW_CLASS = "NoSleepTrayWindowClass";

// Timer ID for auto-start debug feature
#define TIMER_ID_AUTO_START 1000
#define WM_TRAY_UPDATE_CHECK_COMPLETE (WM_APP + 2)

// Notify Icon Notification codes (for NOTIFYICON_VERSION_4)
#ifndef NIN_SELECT
#define NIN_SELECT          0x0400
#endif
#ifndef NIN_KEYSELECT
#define NIN_KEYSELECT       0x0401
#endif
#ifndef NIN_BALLOONSHOW
#define NIN_BALLOONSHOW     0x0402
#endif
#ifndef NIN_BALLOONHIDE
#define NIN_BALLOONHIDE     0x0403
#endif
#ifndef NIN_BALLOONTIMEOUT
#define NIN_BALLOONTIMEOUT  0x0404
#endif
#ifndef NIN_BALLOONUSERCLICK
#define NIN_BALLOONUSERCLICK 0x0405
#endif

// System icon handles for reference (shared icons, should not be destroyed)
static HICON system_icon_default = NULL;
static HICON system_icon_shield = NULL;
static bool update_check_in_progress = false;
static bool update_check_visible = false;
static HWND about_dialog_hwnd = NULL;
static char manual_update_status[128] = "";
// Settings/About procedures share static state; allow only one tray dialog pump.
// Acquire before CreateWindowEx, which can synchronously dispatch window messages.
static bool tray_dialog_open = false;

struct TrayUpdateCheckTask {
    HWND hwnd;
    bool silent;
    bool check_succeeded;
    UpdateInfo info;
    HANDLE thread;
    DWORD thread_id;
};

static bool tray_update_check_begin(void) {
    if (update_check_in_progress) return false;
    update_check_in_progress = true;
    return true;
}

static void tray_update_check_end(void) {
    update_check_in_progress = false;
}

static ULONGLONG get_elapsed_milliseconds(ULONGLONG start_tick64) {
    // GetTickCount64 is monotonic and does not wrap for ~584 million years.
    return GetTickCount64() - start_tick64;
}

// Forward declarations of helper functions
static void tray_create_icons(NoSleepTray* tray);
static void tray_destroy_icons(NoSleepTray* tray);
static void tray_create_menu(NoSleepTray* tray);
static DWORD WINAPI tray_duration_timer(LPVOID lpParam);
static DWORD WINAPI tray_nosleep_thread(LPVOID lpParam);
static bool tray_wait_for_worker_threads(NoSleepTray* tray);
static int tray_show_custom_dialog(NoSleepTray* tray);
static HICON create_colored_icon(COLORREF bg_color, bool draw_z);
static HICON create_numbered_icon(int number);
static HICON load_icon_from_resource(LPCTSTR resource_name, int width, int height);
// Cache one size for the resource, generated glyphs and blink mask.
// The taskbar belongs to Explorer, so its window DPI is independent of our
// hidden window's DPI awareness. Older Windows falls back to system metrics.
static int tray_icon_width = 16;
static int tray_icon_height = 16;

static void initialize_tray_icon_size(void) {
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    typedef int (WINAPI *GetSystemMetricsForDpiFn)(int, UINT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    GetDpiForWindowFn get_dpi = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
    GetSystemMetricsForDpiFn get_metrics = (GetSystemMetricsForDpiFn)GetProcAddress(user32, "GetSystemMetricsForDpi");
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", NULL);
    UINT dpi = get_dpi && taskbar ? get_dpi(taskbar) : 0;
    int width = dpi && get_metrics ? get_metrics(SM_CXSMICON, dpi) : GetSystemMetrics(SM_CXSMICON);
    int height = dpi && get_metrics ? get_metrics(SM_CYSMICON, dpi) : GetSystemMetrics(SM_CYSMICON);
    tray_icon_width = width > 0 ? width : 16;
    tray_icon_height = height > 0 ? height : 16;
}

static void load_gray_and_color_icons(NoSleepTray* tray);
static DWORD WINAPI delayed_sleep_thread(LPVOID lpParam);
static DWORD WINAPI delayed_shutdown_thread(LPVOID lpParam);
static DWORD WINAPI tray_update_check_worker(LPVOID parameter);
static void tray_process_update_check_result(NoSleepTray* tray, bool silent,
                                              bool check_ok, UpdateInfo* info);
static void tray_handle_update_check_complete(NoSleepTray* tray,
                                               TrayUpdateCheckTask* task);
static bool tray_wait_for_update_check(NoSleepTray* tray);
static void tray_set_update_check_visible(NoSleepTray* tray, bool checking);
static void trigger_system_sleep(NoSleepTray* tray);
static void trigger_system_shutdown(NoSleepTray* tray, SessionFinishedAction action);
static bool is_startup_enabled(void);
static bool set_startup_registry(bool enable);
static bool should_check_for_updates(void);
static void tray_setup_update_timer(NoSleepTray* tray);
static void tray_apply_auto_check_interval(NoSleepTray* tray, int interval);
static LRESULT CALLBACK about_dialog_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static bool add_app_to_path(void);
static bool remove_app_from_path(void);
static bool apply_path_preference(bool add_to_path);
static bool path_segment_equal(const wchar_t* a, const wchar_t* b, size_t n);
static wchar_t* get_exe_dir(void);
LRESULT CALLBACK tray_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Forward declaration for notification group edit dialog
void show_notify_group_edit_dialog(HWND hwnd_parent, NotifyGroupManager* mgr, int group_index);

// Font helper: creates a high-quality ClearType font for dialog controls
static HFONT create_dialog_font(int font_size) {
    // Try Segoe UI first (modern Windows UI font), fall back to default
    HFONT hFont = CreateFont(font_size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    if (!hFont) {
        hFont = CreateFont(font_size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, NULL);
    }
    return hFont;
}

// Callback to apply a font to all child controls of a dialog
static BOOL CALLBACK set_child_font_proc(HWND hwndChild, LPARAM lParam) {
    HFONT hFont = (HFONT)lParam;
    if (hFont) {
        SendMessage(hwndChild, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    return TRUE;
}


NoSleepTray* tray_create(void) {
    DEBUG_PRINT("tray_create: allocating tray structure\n");
    NoSleepTray* tray = (NoSleepTray*)malloc(sizeof(NoSleepTray));
    if (!tray) return NULL;
    
    memset(tray, 0, sizeof(NoSleepTray));
    InitializeSRWLock(&tray->delayed_action_lock);
    InitializeSRWLock(&tray->tray_icon_lock);
    InitializeSRWLock(&tray->countdown_icon_cache_lock);
    InitializeConditionVariable(&tray->stop_condition);
    tray->duration_minutes = -1; // Not set
    tray->current_number = -1;   // No numbered icon displayed
    tray->prevent_display = false;
    tray->away_mode = false;
    tray->verbose = false;
    tray->refresh_interval_seconds = 20;
    tray->session_finished_action = SESSION_FINISHED_NONE;
    tray->sleep_after_timeout = false;
    tray->countdown_action = SESSION_FINISHED_NONE;
    tray->countdown_stopping = false;
    tray->start_on_startup = false;
    tray->notification_mode = NOTIFY_ALL;
    tray->add_to_path = false;
    tray->sleep_timer = NULL;
    tray->shutdown_timer = NULL;
    
    tray->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!tray->stop_event) {
        free(tray);
        return NULL;
    }
    
    tray->sleep_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!tray->sleep_stop_event) {
        CloseHandle(tray->stop_event);
        free(tray);
        return NULL;
    }
    
    tray->shutdown_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!tray->shutdown_stop_event) {
        CloseHandle(tray->sleep_stop_event);
        CloseHandle(tray->stop_event);
        free(tray);
        return NULL;
    }
    
    tray->countdown_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!tray->countdown_stop_event) {
        CloseHandle(tray->shutdown_stop_event);
        CloseHandle(tray->sleep_stop_event);
        CloseHandle(tray->stop_event);
        free(tray);
        return NULL;
    }
    
    return tray;
}

static bool tray_wait_for_worker_threads(NoSleepTray* tray) {
    DWORD current_thread_id = GetCurrentThreadId();
    if ((tray->timer_thread && current_thread_id == tray->timer_thread_id) ||
        (tray->nosleep_thread && current_thread_id == tray->nosleep_thread_id)) {
        return false;
    }

    if (tray->timer_thread) {
        WaitForSingleObject(tray->timer_thread, INFINITE);
        CloseHandle(tray->timer_thread);
        tray->timer_thread = NULL;
    }

    if (tray->nosleep_thread) {
        WaitForSingleObject(tray->nosleep_thread, INFINITE);
        CloseHandle(tray->nosleep_thread);
        tray->nosleep_thread = NULL;
    }

    return true;
}

static bool tray_wait_for_update_check(NoSleepTray* tray) {
    if (!tray || !tray->update_check_task) return true;

    TrayUpdateCheckTask* task = tray->update_check_task;
    if (task->thread && GetCurrentThreadId() == task->thread_id) return false;

    if (task->thread) {
        // Do not leave shutdown waiting on the check's synchronous WinHTTP call.
        CancelSynchronousIo(task->thread);
        if (WaitForSingleObject(task->thread, INFINITE) != WAIT_OBJECT_0) {
            DEBUG_LOG("tray_wait_for_update_check: waiting for update worker failed");
            return false;
        }
        CloseHandle(task->thread);
    }

    tray->update_check_task = NULL;
    free(task);
    tray_update_check_end();
    tray_set_update_check_visible(tray, false);
    return true;
}

void tray_destroy(NoSleepTray* tray) {
    if (!tray) return;

    // Prevent a finishing duration thread from publishing a delayed action.
    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    tray->starting_nosleep = true;
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
    
    tray_stop_nosleep(tray, false, false);
    // Join the countdown worker even if it already cleared its active flag.
    tray_stop_countdown(tray);

    if (!tray_wait_for_worker_threads(tray)) {
        DEBUG_LOG("tray_destroy: cannot destroy tray from one of its worker threads");
        return;
    }

    if (!tray_wait_for_update_check(tray)) {
        DEBUG_LOG("tray_destroy: cannot safely wait for update worker");
        return;
    }
    
    if (tray->stop_event) {
        CloseHandle(tray->stop_event);
    }
    
    if (tray->sleep_timer) {
        CloseHandle(tray->sleep_timer);
    }
    
    if (tray->shutdown_timer) {
        CloseHandle(tray->shutdown_timer);
    }
    
    if (tray->sleep_stop_event) {
        CloseHandle(tray->sleep_stop_event);
    }
    
    if (tray->shutdown_stop_event) {
        CloseHandle(tray->shutdown_stop_event);
    }
    
    if (tray->countdown_stop_event) {
        CloseHandle(tray->countdown_stop_event);
    }
    
    tray_destroy_icons(tray);
    
    if (tray->hmenu) {
        DestroyMenu(tray->hmenu);
    }
    
    free(tray);
}

static bool tray_add_icon_to_shell(NoSleepTray* tray) {
    AcquireSRWLockExclusive(&tray->tray_icon_lock);
    tray->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    if (!Shell_NotifyIcon(NIM_ADD, &tray->nid)) {
        DEBUG_PRINT("tray_add_icon_to_shell: Shell_NotifyIcon failed with error %lu\n", GetLastError());
        ReleaseSRWLockExclusive(&tray->tray_icon_lock);
        return false;
    }

    // Set the legacy notification version again after an Explorer restart.
    tray->nid.uVersion = 3; // NOTIFYICON_VERSION
    if (!Shell_NotifyIcon(NIM_SETVERSION, &tray->nid)) {
        DEBUG_PRINT("tray_add_icon_to_shell: NIM_SETVERSION failed with error %lu\n", GetLastError());
    }
    ReleaseSRWLockExclusive(&tray->tray_icon_lock);
    return true;
}

static bool tray_handle_taskbar_created(NoSleepTray* tray, UINT msg) {
    if (!tray || !tray->uTaskbarCreatedMessage || msg != tray->uTaskbarCreatedMessage) {
        return false;
    }

    DEBUG_PRINT("tray_window_proc: Explorer restarted; restoring tray icon\n");
    tray_add_icon_to_shell(tray);
    return true;
}

bool tray_init(NoSleepTray* tray) {
    DEBUG_PRINT("tray_init called\n");
    if (!tray) return false;
    
    // Register window class
    WNDCLASS wc = {0};
    wc.lpfnWndProc = tray_window_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = TRAY_WINDOW_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    
    if (!RegisterClass(&wc)) {
        DEBUG_PRINT("RegisterClass failed\n");
        return false;
    }
    DEBUG_PRINT("RegisterClass succeeded\n");
    
    // Create window (invisible)
    tray->hwnd = CreateWindowEx(
        0,
        TRAY_WINDOW_CLASS,
        "nosleep tray",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL, GetModuleHandle(NULL), tray
    );
    DEBUG_PRINT("CreateWindowEx returned %p\n", tray->hwnd);
    if (!tray->hwnd) {
        DEBUG_PRINT("CreateWindowEx failed\n");
        return false;
    }
    
    // Create icons
    tray_create_icons(tray);
    
    // Create menu
    tray_create_menu(tray);
    if (!tray->hmenu) {
        DEBUG_PRINT("tray_init: Failed to create required context menu\n");
        tray_destroy_icons(tray);
        DestroyWindow(tray->hwnd);
        tray->hwnd = NULL;
        UnregisterClass(TRAY_WINDOW_CLASS, wc.hInstance);
        return false;
    }
    
    // Register unique tray message
    tray->uTrayMessage = RegisterWindowMessage("nosleep_tray_message");
    if (tray->uTrayMessage == 0) {
        DEBUG_PRINT("tray_init: RegisterWindowMessage failed, using default TRAY_ICON_MESSAGE_ID\n");
        tray->uTrayMessage = TRAY_ICON_MESSAGE_ID;
    }
    DEBUG_PRINT("tray_init: Registered tray message ID: %u (0x%X)\n", tray->uTrayMessage, tray->uTrayMessage);

    tray->uTaskbarCreatedMessage = RegisterWindowMessage("TaskbarCreated");
    if (!tray->uTaskbarCreatedMessage) {
        DEBUG_PRINT("tray_init: RegisterWindowMessage(TaskbarCreated) failed; Explorer restart recovery is unavailable\n");
    }
    
    // Setup tray icon
    memset(&tray->nid, 0, sizeof(NOTIFYICONDATA));
    tray->nid.cbSize = sizeof(NOTIFYICONDATA);
    tray->nid.hWnd = tray->hwnd;
    tray->nid.uID = TRAY_ICON_MESSAGE_ID;
    tray->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray->nid.uCallbackMessage = tray->uTrayMessage;
    tray->nid.hIcon = tray->hIconDefault;
    strcpy(tray->nid.szTip, "nosleep - System sleep prevention");
    DEBUG_PRINT("tray_init: NOTIFYICONDATA size=%lu, hWnd=%p, uCallbackMessage=%u\n", tray->nid.cbSize, tray->nid.hWnd, tray->nid.uCallbackMessage);
    
    DEBUG_PRINT("tray_init: Adding tray icon...\n");
    DEBUG_PRINT("tray_init: hWnd=%p, uID=%u, uCallbackMessage=%u\n", tray->hwnd, tray->nid.uID, tray->nid.uCallbackMessage);
    if (!tray_add_icon_to_shell(tray)) {
        DestroyWindow(tray->hwnd);
        return false;
    }
    DEBUG_PRINT("tray_init: Tray icon added successfully\n");
    DEBUG_PRINT("tray_init: Icon added with hWnd=%p, uID=%u, uCallbackMessage=%u\n", tray->nid.hWnd, tray->nid.uID, tray->nid.uCallbackMessage);
    
    // Read startup state from registry
    tray->start_on_startup = is_startup_enabled();

    // Load settings from registry
    tray_load_settings(tray);
    tray_update_session_finished_menu(tray);

    // Apply the saved PATH preference on every startup. A failed update is
    // retried next time because the preference records the desired state.
    if (!apply_path_preference(tray->add_to_path)) {
        DEBUG_PRINT("tray_init: failed to apply PATH preference; it will be retried on next launch\n");
    }

    // Initialize notification groups and migrate old settings
    // Migrate the old mode only if no groups were loaded from the registry.
    notify_groups_init(&tray->notify_groups, tray->notification_mode);

    // Resolve runtime CLI settings after loading preferences, before startup effects.
    if (tray->startup_cli_overrides_set) {
        if (tray->notification_mode_cli_override >= 0) {
            tray->notification_mode = tray->notification_mode_cli_override;
            notify_groups_set_active(&tray->notify_groups, tray->notification_mode);
        }
        if (tray->auto_check_interval_cli_override >= 0) {
            tray->auto_check_interval = tray->auto_check_interval_cli_override;
        }
        if (tray->check_updates_startup_cli_override >= 0) {
            tray->check_updates_on_startup = (tray->check_updates_startup_cli_override != 0);
        }
    }

    // Show the startup notification only after the active notification group is known.
    tray_show_notification(tray, NOTIFY_EVENT_APP_START,
        "nosleep started", "System tray icon created", false);

    // Check for updates on startup if enabled
    if (tray->check_updates_on_startup && should_check_for_updates()) {
        tray_check_for_updates(tray, true);
    }

    // Set up periodic update check timer
    tray_setup_update_timer(tray);

    return true;
}

void tray_run(NoSleepTray* tray) {
    DEBUG_PRINT("tray_run entered\n");
    if (!tray || !tray->hwnd) return;
    
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        DEBUG_PRINT("tray_run: message 0x%04X hwnd=%p\n", msg.message, msg.hwnd);
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

static HICON load_icon_from_resource(LPCTSTR resource_name, int width, int height) {
    HINSTANCE hInstance = GetModuleHandle(NULL);
    if (!hInstance) {
        return NULL;
    }
    
    // First try to load the icon with the specified size
    HICON hIcon = (HICON)LoadImage(hInstance, resource_name, IMAGE_ICON, width, height, LR_DEFAULTCOLOR);
    if (!hIcon) {
        // Fallback: load with default size
        DEBUG_LOG("load_icon_from_resource: failed to load icon %p size %dx%d, trying default size", resource_name, width, height);
        hIcon = (HICON)LoadImage(hInstance, resource_name, IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR | LR_DEFAULTSIZE);
    }
    
    DEBUG_LOG("load_icon_from_resource: icon %p %s", resource_name, hIcon ? "loaded successfully" : "failed to load");
    
    return hIcon;
}

static HICON icon_to_grayscale(HICON hColorIcon) {
    if (!hColorIcon) return NULL;
    
    DEBUG_LOG("icon_to_grayscale: converting icon to grayscale");
    
    ICONINFO iconInfo;
    if (!GetIconInfo(hColorIcon, &iconInfo)) {
        return NULL;
    }
    
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        DEBUG_LOG("icon_to_grayscale: CreateCompatibleDC failed, error=%lu", GetLastError());
        if (hdcScreen) ReleaseDC(NULL, hdcScreen);
        DeleteObject(iconInfo.hbmColor);
        DeleteObject(iconInfo.hbmMask);
        return NULL;
    }
    
    // Get bitmap dimensions
    BITMAP bmp;
    GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bmp);
    
    int width = bmp.bmWidth;
    int height = bmp.bmHeight;
    
    // Create new bitmap for grayscale
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(BITMAPINFO));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    void* pBits = NULL;
    HBITMAP hbmpGray = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hbmpGray) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        DeleteObject(iconInfo.hbmColor);
        DeleteObject(iconInfo.hbmMask);
        return NULL;
    }
    
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hbmpGray);
    
    // Draw color icon to memory DC
    DrawIconEx(hdcMem, 0, 0, hColorIcon, width, height, 0, NULL, DI_NORMAL);
    
    // Complete GDI drawing before accessing DIB pixels directly.
    GdiFlush();
    
    // Convert to grayscale by iterating pixels
    DWORD* pixels = (DWORD*)pBits;
    for (int i = 0; i < width * height; i++) {
        DWORD pixel = pixels[i];
        // A 32-bit BI_RGB DIB stores bytes in BGRA order.
        BYTE r = (pixel >> 16) & 0xFF;
        BYTE g = (pixel >> 8) & 0xFF;
        BYTE b = pixel & 0xFF;
        BYTE a = (pixel >> 24) & 0xFF; // Alpha channel
        
        // Calculate grayscale luminance (standard formula)
        BYTE gray = (BYTE)(0.299f * r + 0.587f * g + 0.114f * b);
        
        pixels[i] = (a << 24) | (gray << 16) | (gray << 8) | gray;
    }
    
    // Create grayscale icon
    ICONINFO grayIconInfo;
    grayIconInfo.fIcon = TRUE;
    grayIconInfo.hbmColor = hbmpGray;
    grayIconInfo.hbmMask = iconInfo.hbmMask; // Reuse mask
    
    HICON hGrayIcon = CreateIconIndirect(&grayIconInfo);
    
    // Cleanup
    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hbmpGray);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    DeleteObject(iconInfo.hbmColor);
    DeleteObject(iconInfo.hbmMask);
    
    DEBUG_LOG("icon_to_grayscale: conversion %s", hGrayIcon ? "succeeded" : "failed");
    
    return hGrayIcon;
}

static void load_gray_and_color_icons(NoSleepTray* tray) {
    DEBUG_LOG("load_gray_and_color_icons: loading icons from resources");
    
    // Load color icon from resource
    tray->hIconActive = load_icon_from_resource(MAKEINTRESOURCE(IDI_APPICON), tray_icon_width, tray_icon_height);
    
    if (!tray->hIconActive) {
        fprintf(stderr, "[nosleep] Failed to load color icon from resource, using fallback\n");
        tray->hIconActive = create_colored_icon(RGB(0, 128, 0), true); // Green with Z
    }
    
    // Create a grayscale version for inactive state
    if (tray->hIconActive) {
        tray->hIconDefault = icon_to_grayscale(tray->hIconActive);
        if (!tray->hIconDefault) {
            // Fallback to gray Z icon if grayscale conversion fails
            tray->hIconDefault = create_colored_icon(RGB(128, 128, 128), true);
        }
    } else {
        // Complete fallback to generated icons
        tray->hIconDefault = create_colored_icon(RGB(128, 128, 128), true);
        tray->hIconActive = create_colored_icon(RGB(0, 128, 0), true);
    }
}

static HICON create_colored_icon(COLORREF bg_color, bool draw_z) {
    // Create a tray-sized color icon with solid background
    const int width = tray_icon_width;
    const int height = tray_icon_height;
    
    // XOR bitmap (color) - 32-bit per pixel (BGRA)
    int xor_row_bytes = width * 4; // 32-bit = 4 bytes per pixel
    int xor_size = xor_row_bytes * height;
    BYTE* xor_bits = (BYTE*)malloc(xor_size);
    if (!xor_bits) return NULL;
    
    // Fill with background color (opaque)
    DWORD color = 0xFF000000 | ((bg_color & 0x000000FF) << 16) | (bg_color & 0x0000FF00) | ((bg_color & 0x00FF0000) >> 16);
    DWORD* pixels = (DWORD*)xor_bits;
    for (int i = 0; i < width * height; i++) {
        pixels[i] = color;
    }
    
    // Draw white Z shape if requested
    if (draw_z) {
        int left = width * 10 / 32;
        int right = width * 22 / 32;
        int top = height * 10 / 32;
        int bottom = height * 22 / 32;
        for (int x = left; x <= right; x++) {
            pixels[top * width + x] = 0xFFFFFFFF;
            pixels[bottom * width + x] = 0xFFFFFFFF;
        }
        for (int y = top; y <= bottom; y++) {
            int x = right - (y - top) * (right - left) / (bottom - top);
            pixels[y * width + x] = 0xFFFFFFFF;
        }
    }
    
    // AND bitmap (mask) - 1-bit per pixel (0 = opaque, 1 = transparent)
    // We want fully opaque icon, so all mask bits = 0
    int and_row_bytes = ((width + 31) / 32) * 4; // 32 pixels -> 4 bytes per row
    int and_size = and_row_bytes * height;
    BYTE* and_bits = (BYTE*)calloc(1, and_size); // All zeros
    if (!and_bits) {
        free(xor_bits);
        return NULL;
    }
    
    // Create icon
    HICON hIcon = CreateIcon(NULL, width, height, 1, 32, and_bits, xor_bits);
    
    free(xor_bits);
    free(and_bits);
    
    return hIcon;
}

static HICON create_numbered_icon(int number) {
    DEBUG_LOG("create_numbered_icon(%d)", number);
    const int width = tray_icon_width;
    const int height = tray_icon_height;
    
    // Create device contexts and bitmaps
    HDC hdc = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdc);
    if (!hdcMem) {
        DEBUG_LOG("create_numbered_icon: CreateCompatibleDC for color bitmap failed, error=%lu", GetLastError());
        if (hdc) ReleaseDC(NULL, hdc);
        return NULL;
    }

    HDC hdcMask = CreateCompatibleDC(hdc);
    if (!hdcMask) {
        DEBUG_LOG("create_numbered_icon: CreateCompatibleDC for mask bitmap failed, error=%lu", GetLastError());
        DeleteDC(hdcMem);
        if (hdc) ReleaseDC(NULL, hdc);
        return NULL;
    }
    
    // Create 32-bit ARGB bitmap for color
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    void* bits;
    HBITMAP hbmpColor = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    DEBUG_LOG("create_numbered_icon: CreateDIBSection %s", hbmpColor ? "succeeded" : "failed");
    if (!hbmpColor) {
        DeleteDC(hdcMem);
        DeleteDC(hdcMask);
        ReleaseDC(NULL, hdc);
        return NULL;
    }
    
    // Create 1-bit mask bitmap (monochrome)
    HBITMAP hbmpMask = CreateBitmap(width, height, 1, 1, NULL);
    DEBUG_LOG("create_numbered_icon: CreateBitmap %s", hbmpMask ? "succeeded" : "failed");
    if (!hbmpMask) {
        DeleteObject(hbmpColor);
        DeleteDC(hdcMem);
        DeleteDC(hdcMask);
        ReleaseDC(NULL, hdc);
        return NULL;
    }
    
    // Select bitmaps into DCs
    HBITMAP oldBmpColor = (HBITMAP)SelectObject(hdcMem, hbmpColor);
    HBITMAP oldBmpMask = (HBITMAP)SelectObject(hdcMask, hbmpMask);
    
    // Fill color bitmap with fully transparent black (alpha = 0)
    DWORD* pixels = (DWORD*)bits;
    for (int i = 0; i < width * height; i++) {
        pixels[i] = 0x00000000; // ARGB: Alpha=0, RGB=0 (transparent black)
    }
    DEBUG_LOG("create_numbered_icon: background pixel[0]=0x%08lX", (unsigned long)pixels[0]);
    
    // Fill mask with white (transparent) initially - mask: 1 = transparent, 0 = opaque
    // We'll set mask to white everywhere (fully transparent), then draw text in black (opaque)
    PatBlt(hdcMask, 0, 0, width, height, WHITENESS);
    
    // Prepare text
    wchar_t text[16];
    if (number < 0) {
        wcscpy(text, L"\u221E");
    } else {
        swprintf(text, sizeof(text) / sizeof(text[0]), L"%d", number);
    }
    
    // Dynamic font scaling algorithm matching Python version
    // Python uses 128x128 canvas with 80pt starting font, max 110x110 area
    int size = width < height ? width : height;
    const int startFontSize = size * 80 / 128;
    const int maxDim = size * 110 / 128;
    
    HFONT hFont = NULL;
    HFONT oldFont = NULL;
    SIZE textSize = {0};
    int fontSize = startFontSize;
    
    // Try to create font with Arial, fallback to default if needed
    hFont = CreateFontW(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
    if (!hFont) {
        // Fallback to system font
        hFont = CreateFontW(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, NULL);
    }
    
    if (hFont) {
        oldFont = (HFONT)SelectObject(hdcMem, hFont);
        
        // Measure text
        GetTextExtentPoint32W(hdcMem, text, (int)wcslen(text), &textSize);
        
        // Scale down if text is too wide or tall
        if (textSize.cx > maxDim || textSize.cy > maxDim) {
            double scaleX = (double)maxDim / textSize.cx;
            double scaleY = (double)maxDim / textSize.cy;
            double scale = scaleX < scaleY ? scaleX : scaleY;
            
            fontSize = (int)(fontSize * scale);
            if (fontSize < 1) fontSize = 1;
            
            // Recreate font with adjusted size
            SelectObject(hdcMem, oldFont);
            DeleteObject(hFont);
            
            hFont = CreateFontW(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
            if (!hFont) {
                hFont = CreateFontW(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, NULL);
            }
            
            if (hFont) {
                oldFont = (HFONT)SelectObject(hdcMem, hFont);
                // Remeasure with new font
                GetTextExtentPoint32W(hdcMem, text, (int)wcslen(text), &textSize);
            }
        }
    }
    
    // Set text properties for color bitmap
    SetTextColor(hdcMem, RGB(255, 255, 255)); // White
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextAlign(hdcMem, TA_LEFT | TA_TOP); // Will calculate position manually
    
    // Calculate centering position (matching Python algorithm)
    int textX = (width - textSize.cx) / 2;
    int textY = (height - textSize.cy) / 2;
    
    // Draw text centered on color bitmap (white text on transparent background)
    TextOutW(hdcMem, textX, textY, text, (int)wcslen(text));
    
    // Complete GDI drawing before accessing DIB pixels directly.
    GdiFlush();
    
    // Capture glyph coverage before adding a one-pixel dark outline.
    BYTE* coverage = (BYTE*)malloc((size_t)width * height);
    if (!coverage) {
        if (oldFont) SelectObject(hdcMem, oldFont);
        if (hFont) DeleteObject(hFont);
        SelectObject(hdcMem, oldBmpColor);
        SelectObject(hdcMask, oldBmpMask);
        DeleteObject(hbmpColor);
        DeleteObject(hbmpMask);
        DeleteDC(hdcMem);
        DeleteDC(hdcMask);
        ReleaseDC(NULL, hdc);
        return NULL;
    }
    for (int i = 0; i < width * height; i++) {
        DWORD color = pixels[i];
        BYTE r = (color >> 16) & 0xFF;
        BYTE g = (color >> 8) & 0xFF;
        BYTE b = color & 0xFF;
        coverage[i] = (BYTE)((r + g + b) / 3);
    }

    // Extend alpha around the white glyph without filling the background.
    // RGB stays premultiplied: the added coverage contributes only black.
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int i = y * width + x;
            BYTE alpha = coverage[i];
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx;
                    int ny = y + dy;
                    if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                        BYTE neighbor = coverage[ny * width + nx];
                        if (neighbor > alpha) alpha = neighbor;
                    }
                }
            }
            pixels[i] = ((DWORD)alpha << 24) | ((DWORD)coverage[i] * 0x00010101);
            if (alpha > 0) {
                SetPixelV(hdcMask, x, y, RGB(0, 0, 0));
            }
        }
    }

    free(coverage);

    // Clean up GDI objects
    if (oldFont) {
        SelectObject(hdcMem, oldFont);
    }
    if (hFont) {
        DeleteObject(hFont);
    }
    SelectObject(hdcMem, oldBmpColor);
    SelectObject(hdcMask, oldBmpMask);
    
    // Create icon from color and mask bitmaps
    ICONINFO iconInfo = {0};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = hbmpColor;
    iconInfo.hbmMask = hbmpMask;
    HICON hIcon = CreateIconIndirect(&iconInfo);
    DEBUG_LOG("create_numbered_icon: CreateIconIndirect %s", hIcon ? "succeeded" : "failed");
    
    // Clean up
    DeleteObject(hbmpColor);
    DeleteObject(hbmpMask);
    DeleteDC(hdcMem);
    DeleteDC(hdcMask);
    ReleaseDC(NULL, hdc);
    
    return hIcon;
}

static HICON create_transparent_icon(void) {
    DEBUG_LOG("create_transparent_icon: creating fully transparent icon");
    
    const int width = tray_icon_width;
    const int height = tray_icon_height;
    
    // Create XOR bitmap (all zeros - black)
    // XOR bitmap is 1-bit per pixel: 0 = black
    int xor_row_bytes = ((width + 15) / 16) * 2; // 32 pixels = 4 bytes per row for 1-bit
    int xor_size = xor_row_bytes * height;
    BYTE* xor_bits = (BYTE*)calloc(1, xor_size); // All zeros = black
    
    // Create AND mask (all ones - transparent)
    // AND mask is 1-bit per pixel: 1 = transparent, 0 = opaque
    int and_row_bytes = ((width + 15) / 16) * 2; // Same calculation
    int and_size = and_row_bytes * height;
    BYTE* and_bits = (BYTE*)malloc(and_size);
    
    if (!xor_bits || !and_bits) {
        free(xor_bits);
        free(and_bits);
        return NULL;
    }
    
    // Fill AND mask with all ones (0xFF = all bits set = transparent)
    memset(and_bits, 0xFF, and_size);
    
    // Create icon using CreateIcon (traditional icon format)
    // Parameters: hInstance, width, height, nPlanes, nBitsPerPixel, ANDbits, XORbits
    HICON hIcon = CreateIcon(NULL, width, height, 1, 1, and_bits, xor_bits);
    
    DEBUG_LOG("create_transparent_icon: CreateIcon %s", hIcon ? "succeeded" : "failed");
    
    free(xor_bits);
    free(and_bits);
    
    // Fallback: try alternative method if CreateIcon fails
    if (!hIcon) {
        DEBUG_LOG("create_transparent_icon: trying alternative method");
        
        // Alternative: create a tray-sized icon with alpha=0
        HDC hdc = GetDC(NULL);
        if (!hdc) {
            DEBUG_LOG("create_transparent_icon: GetDC failed, error=%lu", GetLastError());
            return NULL;
        }
        
        HDC hdcMem = CreateCompatibleDC(hdc);
        if (!hdcMem) {
            DEBUG_LOG("create_transparent_icon: CreateCompatibleDC failed, error=%lu", GetLastError());
            ReleaseDC(NULL, hdc);
            return NULL;
        }
        
        // Create 32-bit ARGB bitmap
        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        
        void* bits;
        HBITMAP hbmpColor = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (hbmpColor) {
            // Fill with fully transparent (alpha=0)
            DWORD* pixels = (DWORD*)bits;
            for (int i = 0; i < width * height; i++) {
                pixels[i] = 0x00000000; // ARGB: Alpha=0, RGB=0
            }
            
            // Create 1-bit mask (all white = transparent)
            HBITMAP hbmpMask = CreateBitmap(width, height, 1, 1, NULL);
            if (hbmpMask) {
                HDC hdcMask = CreateCompatibleDC(hdc);
                if (hdcMask) {
                    HBITMAP oldMask = (HBITMAP)SelectObject(hdcMask, hbmpMask);
                    PatBlt(hdcMask, 0, 0, width, height, WHITENESS);
                    SelectObject(hdcMask, oldMask);
                    DeleteDC(hdcMask);
                }
                
                ICONINFO iconInfo = {0};
                iconInfo.fIcon = TRUE;
                iconInfo.hbmColor = hbmpColor;
                iconInfo.hbmMask = hbmpMask;
                
                hIcon = CreateIconIndirect(&iconInfo);
                if (!hIcon) {
                    DEBUG_LOG("create_transparent_icon: CreateIconIndirect failed, error=%lu", GetLastError());
                }
                
                DeleteObject(hbmpMask);
            } else {
                DEBUG_LOG("create_transparent_icon: CreateBitmap failed, error=%lu", GetLastError());
            }
            
            DeleteObject(hbmpColor);
        } else {
            DEBUG_LOG("create_transparent_icon: CreateDIBSection failed, error=%lu", GetLastError());
        }
        
        if (hdcMem) {
            DeleteDC(hdcMem);
        }
        if (hdc) {
            ReleaseDC(NULL, hdc);
        }
    }
    
    return hIcon;
}

static void tray_create_icons(NoSleepTray* tray) {
    DEBUG_PRINT("tray_create_icons called\n");
    DEBUG_LOG("tray_create_icons: loading icons");
    initialize_tray_icon_size();
    // Load color icon from resource and create grayscale version
    load_gray_and_color_icons(tray);
    
    DEBUG_LOG("tray_create_icons: default icon %s, active icon %s",
            tray->hIconDefault ? "created" : "failed",
            tray->hIconActive ? "created" : "failed");
    
    // Fallback to system icons if custom creation fails
    if (!tray->hIconDefault) {
        // Ensure system icon handles are loaded
        if (!system_icon_default) {
            system_icon_default = LoadIcon(NULL, IDI_APPLICATION);
        }
        tray->hIconDefault = system_icon_default;
    }
    if (!tray->hIconActive) {
        if (!system_icon_shield) {
            system_icon_shield = LoadIcon(NULL, IDI_SHIELD);
        }
        tray->hIconActive = system_icon_shield;
    }
    
    // Numbered icons (0-60) are created lazily on demand in tray_update_icon()
    // to avoid creating every numbered GDI icon at startup.
    
    // Create transparent icon for countdown blink off state
    tray->hIconCountdownBlank = create_transparent_icon();
    DEBUG_LOG("tray_create_icons: transparent icon %s",
            tray->hIconCountdownBlank ? "created" : "failed");
}

static void tray_destroy_icons(NoSleepTray* tray) {
    // Destroy custom icons, but not shared system icons
    if (tray->hIconDefault && tray->hIconDefault != system_icon_default) {
        DestroyIcon(tray->hIconDefault);
    }
    tray->hIconDefault = NULL;

    if (tray->hIconCurrentNumbered) {
        tray_release_numbered_icon(&tray->hIconCurrentNumbered,
                                   tray->current_number, tray->hIconActive,
                                   DestroyIcon);
        tray->current_number = -1;
    }
    
    if (tray->hIconActive && tray->hIconActive != system_icon_shield) {
        DestroyIcon(tray->hIconActive);
    }
    tray->hIconActive = NULL;
    
    AcquireSRWLockExclusive(&tray->countdown_icon_cache_lock);
    tray_destroy_countdown_icon_cache(tray->hIconNumbered, DestroyIcon);
    ReleaseSRWLockExclusive(&tray->countdown_icon_cache_lock);
    
    if (tray->hIconCountdownBlank) {
        DestroyIcon(tray->hIconCountdownBlank);
        tray->hIconCountdownBlank = NULL;
    }
}

static void tray_create_menu(NoSleepTray* tray) {
    HMENU hSubMenu = NULL;
    HMENU hSessionFinishedMenu = NULL;

    tray->hmenu = CreatePopupMenu();
    DEBUG_LOG("tray_create_menu: CreatePopupMenu returned %p", tray->hmenu);
    if (!tray->hmenu) {
        DEBUG_LOG("tray_create_menu: Failed to create menu (error %lu)", GetLastError());
        return;
    }
    
    // Duration submenu
    hSubMenu = CreatePopupMenu();
    DEBUG_LOG("tray_create_menu: CreatePopupMenu submenu returned %p", hSubMenu);
    if (!hSubMenu) {
        DEBUG_LOG("tray_create_menu: Failed to create submenu (error %lu)", GetLastError());
        goto fail;
    }
    
    if (!AppendMenu(hSubMenu, MF_STRING, IDM_START_30MIN, "30 minutes")) {
        goto fail;
    }
    if (!AppendMenu(hSubMenu, MF_STRING, IDM_START_1HOUR, "1 hour")) {
        goto fail;
    }
    if (!AppendMenu(hSubMenu, MF_STRING, IDM_START_2HOURS, "2 hours")) {
        goto fail;
    }
    if (!AppendMenu(hSubMenu, MF_STRING, IDM_START_CUSTOM, "Custom...")) {
        goto fail;
    }
    if (!AppendMenu(hSubMenu, MF_SEPARATOR, 0, NULL)) {
        goto fail;
    }
    if (!AppendMenu(hSubMenu, MF_STRING, IDM_START_INDEFINITE, "Until stopped")) {
        goto fail;
    }
    
    // When finished submenu
    hSessionFinishedMenu = CreatePopupMenu();
    if (!hSessionFinishedMenu) {
        DEBUG_LOG("tray_create_menu: Failed to create When finished submenu (error %lu)", GetLastError());
        goto fail;
    }
    if (!AppendMenu(hSessionFinishedMenu, MF_STRING | (tray->session_finished_action == SESSION_FINISHED_NONE ? MF_CHECKED : MF_UNCHECKED), IDM_SESSION_FINISHED_NONE, "None")) {
        goto fail;
    }
    if (!AppendMenu(hSessionFinishedMenu, MF_STRING | (tray->session_finished_action == SESSION_FINISHED_SHUTDOWN ? MF_CHECKED : MF_UNCHECKED), IDM_SESSION_FINISHED_SHUTDOWN, "Shutdown (force)")) {
        goto fail;
    }
    if (!AppendMenu(hSessionFinishedMenu, MF_STRING | (tray->session_finished_action == SESSION_FINISHED_SHUTDOWN_GRACEFUL ? MF_CHECKED : MF_UNCHECKED), IDM_SESSION_FINISHED_SHUTDOWN_GRACEFUL, "Shutdown (graceful)")) {
        goto fail;
    }
    if (!AppendMenu(hSessionFinishedMenu, MF_STRING | (tray->session_finished_action == SESSION_FINISHED_SLEEP ? MF_CHECKED : MF_UNCHECKED), IDM_SESSION_FINISHED_SLEEP, "Sleep")) {
        goto fail;
    }
    
    // Create main menu item text based on current selection
    char finished_text[64];
    switch (tray->session_finished_action) {
        case SESSION_FINISHED_NONE:
            snprintf(finished_text, sizeof(finished_text), "When finished (None)");
            break;
        case SESSION_FINISHED_SHUTDOWN:
            snprintf(finished_text, sizeof(finished_text), "When finished (Shutdown: force)");
            break;
        case SESSION_FINISHED_SHUTDOWN_GRACEFUL:
            snprintf(finished_text, sizeof(finished_text), "When finished (Shutdown: graceful)");
            break;
        case SESSION_FINISHED_SLEEP:
            snprintf(finished_text, sizeof(finished_text), "When finished (Sleep)");
            break;
        default:
            snprintf(finished_text, sizeof(finished_text), "When finished");
            break;
    }
    
    // Build main menu
    // 1. Primary actions first (When finished, Set Duration)
    // 2. Stop (no separator between Set Duration and Stop)
    // 3. --- separator ---
    // 4. Settings, Check for Updates, About (grouped together)
    // 5. --- separator ---
    // 6. Exit
    if (!AppendMenu(tray->hmenu, MF_STRING | MF_POPUP, (UINT_PTR)hSessionFinishedMenu, finished_text)) {
        goto fail;
    }
    hSessionFinishedMenu = NULL; // The parent menu now owns this submenu.
    if (!AppendMenu(tray->hmenu, MF_SEPARATOR, 0, NULL)) {
        goto fail;
    }
    if (!AppendMenu(tray->hmenu, MF_STRING | MF_POPUP, (UINT_PTR)hSubMenu, "Start for...")) {
        goto fail;
    }
    hSubMenu = NULL; // The parent menu now owns this submenu.
    if (!AppendMenu(tray->hmenu, MF_STRING | MF_GRAYED, IDM_STOP, "Stop")) {
        goto fail;
    }
    if (!AppendMenu(tray->hmenu, MF_SEPARATOR, 0, NULL)) {
        goto fail;
    }
    // Group: Settings, Check for Updates, About
    if (!AppendMenu(tray->hmenu, MF_STRING, IDM_SETTINGS, "Settings...")) {
        goto fail;
    }
    if (!AppendMenu(tray->hmenu, MF_STRING, IDM_CHECK_UPDATES, "Check for Updates...")) {
        goto fail;
    }
    if (!AppendMenu(tray->hmenu, MF_STRING | MF_GRAYED, IDM_REVIEW_UPDATE, "Review Available Update...")) {
        goto fail;
    }
    if (!AppendMenu(tray->hmenu, MF_STRING, IDM_ABOUT, "About")) {
        goto fail;
    }
    if (!AppendMenu(tray->hmenu, MF_SEPARATOR, 0, NULL)) {
        goto fail;
    }
    if (!AppendMenu(tray->hmenu, MF_STRING, IDM_EXIT, "Exit")) {
        goto fail;
    }
    DEBUG_LOG("tray_create_menu: Menu created successfully");
    return;

fail:
    DEBUG_LOG("tray_create_menu: Menu construction failed (error %lu)", GetLastError());
    if (hSessionFinishedMenu) {
        DestroyMenu(hSessionFinishedMenu);
    }
    if (hSubMenu) {
        DestroyMenu(hSubMenu);
    }
    DestroyMenu(tray->hmenu);
    tray->hmenu = NULL;
}

void tray_start_nosleep(NoSleepTray* tray, int duration_minutes) {
    DEBUG_LOG("tray_start_nosleep(%d)", duration_minutes);
    if (!tray) {
        return;
    }
    
    // Claim the transition so an expiring duration timer cannot publish an action
    // between cleanup and publication of the replacement session.
    for (;;) {
        AcquireSRWLockExclusive(&tray->delayed_action_lock);
        if (!ATOMIC_LOAD_BOOL(&tray->stopping) && !tray->starting_nosleep) {
            tray->starting_nosleep = true;
            ReleaseSRWLockExclusive(&tray->delayed_action_lock);
            break;
        }
        ReleaseSRWLockExclusive(&tray->delayed_action_lock);
        Sleep(10);
    }

    // Stop the current session and cancel any delayed action left by its expiry.
    tray_stop_nosleep(tray, false, true);

    // A worker that owned the previous stop may retain its handle until it exits.
    if (!tray_wait_for_worker_threads(tray)) {
        AcquireSRWLockExclusive(&tray->delayed_action_lock);
        tray->starting_nosleep = false;
        ReleaseSRWLockExclusive(&tray->delayed_action_lock);
        return;
    }

    // Do not publish a new session while stop cleanup is still running.
    for (;;) {
        AcquireSRWLockExclusive(&tray->delayed_action_lock);
        if (!ATOMIC_LOAD_BOOL(&tray->stopping)) {
            break;
        }
        ReleaseSRWLockExclusive(&tray->delayed_action_lock);
        Sleep(10);
    }

    // Keep the worker from handling failure until this session's thread handles
    // and IDs have been published.
    tray->duration_minutes = duration_minutes;
    ATOMIC_STORE_BOOL(&tray->is_running, true);
    ATOMIC_STORE_BOOL(&tray->duration_expired, false);
    ATOMIC_STORE_BOOL(&tray->stopping, false);
    ATOMIC_STORE_BOOL(&tray->core_init_failed, false);
    ATOMIC_STORE_BOOL(&tray->core_init_succeeded, false);
    ATOMIC_STORE_BOOL(&tray->nosleep_run_failed, false);
    tray->session_action_cancelled = false;
    tray->timer_thread_id = 0;
    tray->nosleep_thread_id = 0;
    tray->start_tick64 = GetTickCount64();
    ResetEvent(tray->stop_event);
    
    // Update icon
    tray_update_icon(tray);
    
    // Show notification
    if (duration_minutes > 0) {
        char message[256];
        sprintf(message, "Preventing system sleep for %d minutes", duration_minutes);
        tray_show_notification(tray, NOTIFY_EVENT_SESSION_START, "Starting", message, false);
    } else {
        tray_show_notification(tray, NOTIFY_EVENT_SESSION_START,
            "Starting", "Preventing system sleep indefinitely", false);
    }
    
    // Start nosleep thread
    tray->nosleep_thread = CreateThread(
        NULL, 0, tray_nosleep_thread, tray, 0, NULL
    );
    if (tray->nosleep_thread) {
        tray->nosleep_thread_id = GetThreadId(tray->nosleep_thread);
    } else {
        // Thread creation failed, restore state and notify user
        ATOMIC_STORE_BOOL(&tray->is_running, false);
        tray->duration_minutes = -1;
        tray->starting_nosleep = false;
        tray_update_icon(tray);
        ReleaseSRWLockExclusive(&tray->delayed_action_lock);
        tray_show_notification(tray, NOTIFY_EVENT_ERROR,
            "Error", "Failed to create nosleep thread", true);
        return;
    }
    
    // Start duration timer thread if duration is set
    if (duration_minutes > 0) {
        tray->timer_thread = CreateThread(
            NULL, 0, tray_duration_timer, tray, 0, NULL
        );
        if (tray->timer_thread) {
            tray->timer_thread_id = GetThreadId(tray->timer_thread);
        } else {
            // Timer thread creation failed, stop nosleep thread and restore state
            DWORD error = GetLastError();
            ReleaseSRWLockExclusive(&tray->delayed_action_lock);
            tray_stop_nosleep(tray, false, true); // suppress notification

            AcquireSRWLockExclusive(&tray->delayed_action_lock);
            tray->starting_nosleep = false;
            ReleaseSRWLockExclusive(&tray->delayed_action_lock);
            
            char error_msg[256];
            sprintf(error_msg, "Failed to create timer thread. Error code: %lu", error);
            tray_show_notification(tray, NOTIFY_EVENT_ERROR, "Error", error_msg, true);
            return;
        }
    }

    tray->starting_nosleep = false;
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
    
    // Update stop menu item text
    tray_update_stop_menu_item(tray);
}

// Called with delayed_action_lock held. A countdown worker reserves startup
// under this lock while updating the tray outside it; Stop waits for that
// update to finish before signaling cancellation.
static void tray_wait_for_delayed_countdown_start(NoSleepTray* tray) {
    while (tray->delayed_countdown_starting) {
        SleepConditionVariableSRW(&tray->stop_condition,
                                  &tray->delayed_action_lock,
                                  INFINITE, 0);
    }
}

static bool tray_stop_nosleep_for_session(NoSleepTray* tray,
                                          DWORD expected_thread_id,
                                          bool timer_expired,
                                          bool suppress_notification) {
    DEBUG_LOG("tray_stop_nosleep called with timer_expired=%s, suppress_notification=%s", timer_expired ? "true" : "false", suppress_notification ? "true" : "false");
    
    // Serialize stop cleanup and let concurrent callers wait for the active stop.
    if (!tray) return false;
    
    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    bool is_nosleep_thread = expected_thread_id != 0 &&
                             tray->nosleep_thread_id == expected_thread_id;
    bool is_timer_thread = expected_thread_id != 0 &&
                           tray->timer_thread_id == expected_thread_id;
    if (expected_thread_id != 0 && !is_nosleep_thread && !is_timer_thread) {
        ReleaseSRWLockExclusive(&tray->delayed_action_lock);
        DEBUG_LOG("tray_stop_nosleep: ignoring cleanup from an ended session");
        return false;
    }

    tray_wait_for_delayed_countdown_start(tray);

    if (is_nosleep_thread) {
        ATOMIC_STORE_BOOL(&tray->core_init_failed, true);
    }

    // Cancel even when expiry cleanup has not yet published its delayed action.
    if (expected_thread_id == 0 && !timer_expired) {
        tray->session_action_cancelled = true;
    }

    DWORD current_thread_id = GetCurrentThreadId();
    bool was_stopping = ATOMIC_EXCHANGE_BOOL(&tray->stopping, true);
    if (was_stopping) {
        // Calls made again by the cleanup owner are re-entrant. Session worker
        // callbacks must also return without waiting, because cleanup joins them.
        bool is_reentrant = tray->stopping_thread_id == current_thread_id;
        bool is_stop_worker = current_thread_id == tray->nosleep_thread_id ||
                              current_thread_id == tray->timer_thread_id;
        bool waited_for_stop = !is_reentrant && !is_stop_worker;
        if (waited_for_stop) {
            while (ATOMIC_LOAD_BOOL(&tray->stopping)) {
                SleepConditionVariableSRW(&tray->stop_condition,
                                          &tray->delayed_action_lock,
                                          INFINITE, 0);
            }
        }
        ReleaseSRWLockExclusive(&tray->delayed_action_lock);
        DEBUG_LOG("tray_stop_nosleep: duplicate stop %s",
                  waited_for_stop ? "waited for active cleanup" :
                  is_reentrant ? "reentrant call returned" : "stop worker returned");
        return expected_thread_id != 0 && is_reentrant;
    }
    tray->stopping_thread_id = current_thread_id;

    bool has_work = tray_stop_has_work(ATOMIC_LOAD_BOOL(&tray->is_running),
                                       ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active),
                                       tray->sleep_timer != NULL,
                                       tray->shutdown_timer != NULL);
    if (is_nosleep_thread && !has_work) {
        tray->stopping_thread_id = 0;
        ATOMIC_STORE_BOOL(&tray->stopping, false);
        WakeAllConditionVariable(&tray->stop_condition);
        ReleaseSRWLockExclusive(&tray->delayed_action_lock);
        DEBUG_LOG("tray_stop_nosleep: ended session has no cleanup work");
        return true;
    }
    if (!has_work) {
        DEBUG_LOG("tray_stop_nosleep: no active session, countdown, or delayed action; resetting stopping flag and returning");
        tray->stopping_thread_id = 0;
        ATOMIC_STORE_BOOL(&tray->stopping, false);
        WakeAllConditionVariable(&tray->stop_condition);
        ReleaseSRWLockExclusive(&tray->delayed_action_lock);
        return is_nosleep_thread;
    }

    // Preserve countdown state before its worker observes cancellation and clears it.
    bool was_countdown_active = ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active);

    HANDLE sleep_timer = tray->sleep_timer;
    HANDLE shutdown_timer = tray->shutdown_timer;
    bool sleep_pending = sleep_timer != NULL && !tray->sleep_action_claimed;
    bool shutdown_pending = shutdown_timer != NULL && !tray->shutdown_action_claimed;
    bool was_delayed_action_pending = sleep_pending || shutdown_pending;
    SessionFinishedAction countdown_action = tray->countdown_action;
    bool saved_action_matches_pending =
        ((countdown_action == SESSION_FINISHED_SHUTDOWN ||
          countdown_action == SESSION_FINISHED_SHUTDOWN_GRACEFUL) && shutdown_timer) ||
        (countdown_action == SESSION_FINISHED_SLEEP && sleep_timer);
    if (!saved_action_matches_pending) {
        // The pending timer identifies the action when the saved countdown type is stale.
        if (shutdown_timer) {
            countdown_action = tray->shutdown_action;
        } else if (sleep_timer) {
            countdown_action = SESSION_FINISHED_SLEEP;
        }
    }
    tray->sleep_timer = NULL;
    tray->shutdown_timer = NULL;
    if (sleep_pending) SetEvent(tray->sleep_stop_event);
    if (shutdown_pending) SetEvent(tray->shutdown_stop_event);

    ATOMIC_STORE_BOOL(&tray->is_running, false);
    SetEvent(tray->stop_event);
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
    
    // Wait for threads to finish
    if (tray->timer_thread) {
        DWORD current_thread_id = GetCurrentThreadId();
        if (current_thread_id != tray->timer_thread_id) {
            if (WaitForSingleObject(tray->timer_thread, 2000) == WAIT_OBJECT_0) {
                CloseHandle(tray->timer_thread);
                tray->timer_thread = NULL;
            }
        }
    }
    
    if (tray->nosleep_thread) {
        DWORD current_thread_id = GetCurrentThreadId();
        if (current_thread_id != tray->nosleep_thread_id) {
            // Resolve the core result before reporting or starting any follow-up action.
            if (WaitForSingleObject(tray->nosleep_thread, INFINITE) == WAIT_OBJECT_0) {
                CloseHandle(tray->nosleep_thread);
                tray->nosleep_thread = NULL;
            }
        }
    }
    
    // Cancel and wait for sleep timer thread if active
    if (sleep_timer) {
        DWORD current_thread_id = GetCurrentThreadId();
        DWORD sleep_timer_thread_id = GetThreadId(sleep_timer);
        if (current_thread_id != sleep_timer_thread_id) {
            WaitForSingleObject(sleep_timer, INFINITE);
        }
        CloseHandle(sleep_timer);
        
        // Reset the event for future use
        ResetEvent(tray->sleep_stop_event);
    }
    
    // Cancel and wait for shutdown timer thread if active
    if (shutdown_timer) {
        DWORD current_thread_id = GetCurrentThreadId();
        DWORD shutdown_timer_thread_id = GetThreadId(shutdown_timer);
        if (current_thread_id != shutdown_timer_thread_id) {
            WaitForSingleObject(shutdown_timer, INFINITE);
        }
        CloseHandle(shutdown_timer);
        
        // Reset the event for future use
        ResetEvent(tray->shutdown_stop_event);
    }
    
    // Cancel sleep if active
    DEBUG_LOG("tray_stop_nosleep: calling tray_stop_countdown, delayed_sleep_countdown_active=%s", 
            ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active) ? "true" : "false");
    tray_stop_countdown(tray);

    // Reset execution state to allow Windows to sleep normally
    // This ensures that any previous ES_SYSTEM_REQUIRED flags are cleared
    SetThreadExecutionState(ES_CONTINUOUS);
    
    // Calculate elapsed time from the monotonic session start tick.
    ULONGLONG elapsed_seconds = get_elapsed_milliseconds(tray->start_tick64) / 1000;
    
    int hours = (int)(elapsed_seconds / 3600);
    int minutes = (int)((elapsed_seconds % 3600) / 60);
    int seconds = (int)(elapsed_seconds % 60);
    
    char message[256];
    DEBUG_LOG("tray_stop_nosleep: timer_expired=%s, duration_expired=%s, was_countdown_active=%s, was_delayed_action_pending=%s", timer_expired ? "true" : "false", ATOMIC_LOAD_BOOL(&tray->duration_expired) ? "true" : "false", was_countdown_active ? "true" : "false", was_delayed_action_pending ? "true" : "false");
    
    // Determine which notification to show
    bool nosleep_run_failed = ATOMIC_LOAD_BOOL(&tray->nosleep_run_failed);
    if (!suppress_notification || (nosleep_run_failed && !is_nosleep_thread)) {
        if (nosleep_run_failed) {
            tray_show_notification(tray, NOTIFY_EVENT_ERROR,
                "Sleep prevention failed",
                "Sleep prevention stopped after repeated refresh failures.", true);
        } else if (was_countdown_active || was_delayed_action_pending) {
            // Countdown or delayed action was cancelled
            DEBUG_LOG("tray_stop_nosleep: showing countdown cancellation notification");
            if (countdown_action == SESSION_FINISHED_SHUTDOWN ||
                countdown_action == SESSION_FINISHED_SHUTDOWN_GRACEFUL) {
                tray_show_notification(tray, NOTIFY_EVENT_COUNTDOWN_CANCEL,
                    "Shutdown cancelled", "System shutdown has been cancelled", true);
            } else {
                tray_show_notification(tray, NOTIFY_EVENT_COUNTDOWN_CANCEL,
                    "Sleep cancelled", "System sleep has been cancelled", true);
            }
        } else if (ATOMIC_LOAD_BOOL(&tray->duration_expired)) {
            // Timer expired (nosleep session finished naturally)
            DEBUG_LOG("tray_stop_nosleep: showing Time's up! notification");
            if (hours > 0) {
                sprintf(message, "Sleep prevention stopped\nDuration: %dh %dm", hours, minutes);
            } else {
                sprintf(message, "Sleep prevention stopped\nDuration: %dm %ds", minutes, seconds);
            }
            tray_show_notification(tray, NOTIFY_EVENT_TIMER_EXPIRED, "Time's up!", message, false);
        } else {
            // Nosleep session manually stopped
            DEBUG_LOG("tray_stop_nosleep: showing Stopped notification");
            if (hours > 0) {
                sprintf(message, "Sleep prevention manually stopped\nTotal duration: %dh %dm", hours, minutes);
            } else {
                sprintf(message, "Sleep prevention manually stopped\nTotal duration: %dm %ds", minutes, seconds);
            }
            tray_show_notification(tray, NOTIFY_EVENT_SESSION_STOP, "Stopped", message, false);
        }
    } else {
        DEBUG_LOG("tray_stop_nosleep: notification suppressed");
    }
    
    // Update stop menu item text
    tray_update_stop_menu_item(tray);
    
    // Update icon
    tray_update_icon(tray);
    
    // Reset stopping flag
    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    tray->stopping_thread_id = 0;
    ATOMIC_STORE_BOOL(&tray->stopping, false);
    WakeAllConditionVariable(&tray->stop_condition);
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
    return true;
}

void tray_stop_nosleep(NoSleepTray* tray, bool timer_expired, bool suppress_notification) {
    (void)tray_stop_nosleep_for_session(tray, 0, timer_expired, suppress_notification);
}

static bool tray_has_stop_work(NoSleepTray* tray) {
    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    bool has_work = tray_stop_has_work(ATOMIC_LOAD_BOOL(&tray->is_running),
                                       ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active),
                                       tray->sleep_timer != NULL,
                                       tray->shutdown_timer != NULL);
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
    return has_work;
}

static void tray_reap_delayed_action_handle(NoSleepTray* tray, HANDLE* timer_handle) {
    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    HANDLE completed_timer = *timer_handle;
    *timer_handle = NULL;
    if (completed_timer) CloseHandle(completed_timer);
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
}

static DWORD WINAPI tray_duration_timer(LPVOID lpParam) {
    NoSleepTray* tray = (NoSleepTray*)lpParam;
    DWORD timer_thread_id = GetCurrentThreadId();

    // A duration thread may be scheduled after its session has been replaced.
    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    bool is_current_session = (tray->timer_thread_id == timer_thread_id);
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
    if (!is_current_session) {
        DEBUG_LOG("tray_duration_timer: ignoring timer from an ended session");
        return 0;
    }
    
    DEBUG_LOG("tray_duration_timer: started, duration_minutes=%d, session_finished_action=%d",
            tray->duration_minutes, tray->session_finished_action);
    
    if (tray->duration_minutes <= 0) {
        DEBUG_LOG("tray_duration_timer: duration_minutes=%d, returning early", tray->duration_minutes);
        return 0;
    }
    
    ULONGLONG duration_ms = (ULONGLONG)tray->duration_minutes * 60 * 1000;
    
    while (ATOMIC_LOAD_BOOL(&tray->is_running)) {
        ULONGLONG elapsed_ms = get_elapsed_milliseconds(tray->start_tick64);
        
        if (elapsed_ms >= duration_ms) {
            SessionFinishedAction finished_action = tray->session_finished_action;
            if (finished_action != SESSION_FINISHED_NONE) {
                // Do not honor a follow-up action until the NoSleep core is known to exist.
                for (;;) {
                    AcquireSRWLockExclusive(&tray->delayed_action_lock);
                    bool is_current_session = (tray->timer_thread_id == timer_thread_id);
                    bool is_running = ATOMIC_LOAD_BOOL(&tray->is_running);
                    bool core_init_failed = ATOMIC_LOAD_BOOL(&tray->core_init_failed);
                    bool core_init_succeeded = ATOMIC_LOAD_BOOL(&tray->core_init_succeeded);
                    bool nosleep_run_failed = ATOMIC_LOAD_BOOL(&tray->nosleep_run_failed);
                    bool session_starting = tray->starting_nosleep;
                    ReleaseSRWLockExclusive(&tray->delayed_action_lock);

                    if (!is_current_session || !is_running || core_init_failed ||
                        nosleep_run_failed || session_starting) {
                        DEBUG_LOG("tray_duration_timer: abandoning action while core initialization or session state changes");
                        return 0;
                    }
                    if (core_init_succeeded) {
                        break;
                    }
                    Sleep(10);
                }
            }

            // Duration reached
            DEBUG_LOG("tray_duration_timer: duration reached, stopping with timer_expired=true");
            ATOMIC_STORE_BOOL(&tray->duration_expired, true);
            
            DEBUG_LOG("tray_duration_timer: elapsed=%llu ms, duration=%llu ms, calling tray_stop_nosleep",
                    elapsed_ms, duration_ms);
            
            // Check what action to take when session finishes
            bool suppress_notification = (finished_action != SESSION_FINISHED_NONE);
            if (!tray_stop_nosleep_for_session(
                    tray, timer_thread_id, true, suppress_notification)) {
                return 0;
            }
            
            switch (finished_action) {
                case SESSION_FINISHED_SLEEP: {
                    DEBUG_LOG("tray_duration_timer: session_finished_action=SLEEP, starting delayed sleep thread");

                    // Serialize action startup against NoSleep creation failure cleanup.
                    HANDLE sleep_timer = NULL;
                    bool core_init_failed;
                    bool core_init_succeeded;
                    bool nosleep_run_failed;
                    bool stale_session;
                    bool session_starting;
                    AcquireSRWLockExclusive(&tray->delayed_action_lock);
                    stale_session = (tray->timer_thread_id != timer_thread_id) ||
                                    tray->session_action_cancelled;
                    core_init_failed = ATOMIC_LOAD_BOOL(&tray->core_init_failed);
                    core_init_succeeded = ATOMIC_LOAD_BOOL(&tray->core_init_succeeded);
                    nosleep_run_failed = ATOMIC_LOAD_BOOL(&tray->nosleep_run_failed);
                    session_starting = tray->starting_nosleep;
                    if (!stale_session && !session_starting && !core_init_failed &&
                        !nosleep_run_failed && core_init_succeeded) {
                        ResetEvent(tray->sleep_stop_event);
                        tray->sleep_action_claimed = false;
                        tray->sleep_timer = CreateThread(
                            NULL, 0, delayed_sleep_thread, tray, 0, NULL
                        );
                        sleep_timer = tray->sleep_timer;
                    }
                    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
                    if (stale_session) {
                        DEBUG_LOG("tray_duration_timer: skipping sleep action from an ended session");
                        break;
                    }
                    if (session_starting) {
                        DEBUG_LOG("tray_duration_timer: skipping sleep action during session replacement");
                        break;
                    }
                    if (core_init_failed) {
                        DEBUG_LOG("tray_duration_timer: skipping sleep action after NoSleep initialization failure");
                        break;
                    }
                    if (nosleep_run_failed) {
                        DEBUG_LOG("tray_duration_timer: skipping sleep action after NoSleep worker failure");
                        break;
                    }
                    if (!core_init_succeeded) {
                        DEBUG_LOG("tray_duration_timer: skipping sleep action before NoSleep initialization completed");
                        break;
                    }
                    if (!sleep_timer) {
                        DEBUG_LOG("tray_duration_timer: failed to create sleep timer thread");
                        // Show error notification
                        tray_show_notification(tray, NOTIFY_EVENT_ERROR,
                            "Error", "Failed to start sleep timer", true);
                    } else {
                        DEBUG_LOG("tray_duration_timer: delayed sleep thread created successfully");
                        // Countdown display will be started by delayed_sleep_thread
                    }
                    break;
                }

                case SESSION_FINISHED_SHUTDOWN_GRACEFUL:
                case SESSION_FINISHED_SHUTDOWN: {
                    DEBUG_LOG("tray_duration_timer: session_finished_action=SHUTDOWN, starting delayed shutdown thread");

                    // Serialize action startup against NoSleep creation failure cleanup.
                    HANDLE shutdown_timer = NULL;
                    bool shutdown_core_init_failed;
                    bool shutdown_core_init_succeeded;
                    bool shutdown_nosleep_run_failed;
                    bool shutdown_stale_session;
                    bool shutdown_session_starting;
                    AcquireSRWLockExclusive(&tray->delayed_action_lock);
                    shutdown_stale_session = (tray->timer_thread_id != timer_thread_id) ||
                                    tray->session_action_cancelled;
                    shutdown_core_init_failed = ATOMIC_LOAD_BOOL(&tray->core_init_failed);
                    shutdown_core_init_succeeded = ATOMIC_LOAD_BOOL(&tray->core_init_succeeded);
                    shutdown_nosleep_run_failed = ATOMIC_LOAD_BOOL(&tray->nosleep_run_failed);
                    shutdown_session_starting = tray->starting_nosleep;
                    if (!shutdown_stale_session && !shutdown_session_starting &&
                        !shutdown_core_init_failed && !shutdown_nosleep_run_failed &&
                        shutdown_core_init_succeeded) {
                        ResetEvent(tray->shutdown_stop_event);
                        tray->shutdown_action = finished_action;
                        tray->shutdown_action_claimed = false;
                        tray->shutdown_timer = CreateThread(
                            NULL, 0, delayed_shutdown_thread, tray, 0, NULL
                        );
                        shutdown_timer = tray->shutdown_timer;
                    }
                    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
                    if (shutdown_stale_session) {
                        DEBUG_LOG("tray_duration_timer: skipping shutdown action from an ended session");
                        break;
                    }
                    if (shutdown_session_starting) {
                        DEBUG_LOG("tray_duration_timer: skipping shutdown action during session replacement");
                        break;
                    }
                    if (shutdown_core_init_failed) {
                        DEBUG_LOG("tray_duration_timer: skipping shutdown action after NoSleep initialization failure");
                        break;
                    }
                    if (shutdown_nosleep_run_failed) {
                        DEBUG_LOG("tray_duration_timer: skipping shutdown action after NoSleep worker failure");
                        break;
                    }
                    if (!shutdown_core_init_succeeded) {
                        DEBUG_LOG("tray_duration_timer: skipping shutdown action before NoSleep initialization completed");
                        break;
                    }
                    if (!shutdown_timer) {
                        DEBUG_LOG("tray_duration_timer: failed to create shutdown timer thread");
                        // Show error notification
                        tray_show_notification(tray, NOTIFY_EVENT_ERROR,
                            "Error", "Failed to start shutdown timer", true);
                    } else {
                        DEBUG_LOG("tray_duration_timer: delayed shutdown thread created successfully");
                        // Countdown display will be started by delayed_shutdown_thread
                    }
                    break;
                }
                    
                case SESSION_FINISHED_NONE:
                default:
                    DEBUG_LOG("tray_duration_timer: session_finished_action=NONE, no action");
                    break;
            }
            break;
        }
        
        // Update icon every second
        tray_update_icon(tray);
        
        // Wait up to 1 second or until stop event
        WaitForSingleObject(tray->stop_event, 1000);
        if (WaitForSingleObject(tray->stop_event, 0) == WAIT_OBJECT_0) {
            break;
        }
    }
    
    return 0;
}

static bool tray_mode_for_run(bool preference, bool override_set, bool override_value) {
    return override_set ? override_value : preference;
}

static DWORD WINAPI tray_nosleep_thread(LPVOID lpParam) {
    NoSleepTray* tray = (NoSleepTray*)lpParam;
    
    // Create NoSleep instance
    NoSleep* ns = nosleep_create();
    if (!ns) {
        bool is_current_session = tray_stop_nosleep_for_session(
            tray, GetCurrentThreadId(), false, true
        );
        if (is_current_session) {
            tray_show_notification(tray, NOTIFY_EVENT_ERROR,
                "Error", "Failed to create NoSleep instance", true);
        } else {
            DEBUG_LOG("tray_nosleep_thread: ignoring core initialization failure from an ended session");
        }
        return 1;
    }

    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    bool is_current_session = (tray->nosleep_thread_id == GetCurrentThreadId());
    bool is_running = ATOMIC_LOAD_BOOL(&tray->is_running);
    if (is_current_session && is_running) {
        ATOMIC_STORE_BOOL(&tray->core_init_succeeded, true);
    }
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
    if (!is_current_session || !is_running) {
        nosleep_destroy(ns);
        return 0;
    }

    bool prevent_display = tray_mode_for_run(
        tray->prevent_display,
        tray->prevent_display_cli_override_set,
        tray->prevent_display_cli_override
    );
    bool away_mode = tray_mode_for_run(
        tray->away_mode,
        tray->away_mode_cli_override_set,
        tray->away_mode_cli_override
    );
    
    // Run nosleep
    int result = nosleep_run(
        ns,
        0, // duration_minutes (0 = indefinite, controlled by timer thread)
        tray->refresh_interval_seconds, // interval_seconds
        prevent_display, // prevent_display
        away_mode, // away_mode
        tray_mode_for_run(tray->verbose, tray->verbose_cli_override_set,
                          tray->verbose_cli_override), // verbose
        tray->stop_event // external stop event
    );

    if (result != 0) {
        AcquireSRWLockExclusive(&tray->delayed_action_lock);
        if (tray->nosleep_thread_id == GetCurrentThreadId()) {
            ATOMIC_STORE_BOOL(&tray->nosleep_run_failed, true);
        }
        ReleaseSRWLockExclusive(&tray->delayed_action_lock);
    }
    
    nosleep_destroy(ns);
    
    // If nosleep completed (duration reached or error), update tray state
    DEBUG_LOG("tray_nosleep_thread: nosleep completed, tray->is_running=%s", ATOMIC_LOAD_BOOL(&tray->is_running) ? "true" : "false");
    bool session_stopped = false;
    if (ATOMIC_LOAD_BOOL(&tray->is_running)) {
        DEBUG_LOG("tray_nosleep_thread: calling tray_stop_nosleep with timer_expired=false");
        session_stopped = tray_stop_nosleep_for_session(
            tray, GetCurrentThreadId(), false, result != 0
        );
    }

    if (result != 0 && session_stopped) {
        tray_show_notification(tray, NOTIFY_EVENT_ERROR,
            "Sleep prevention failed",
            "Sleep prevention stopped after repeated refresh failures.", true);
    }
    
    return result;
}

static void trigger_system_sleep(NoSleepTray* tray) {
    DEBUG_LOG("trigger_system_sleep: putting system to sleep");

    // First, reset execution state to ensure Windows can sleep
    SetThreadExecutionState(ES_CONTINUOUS);
    DEBUG_LOG("trigger_system_sleep: execution state reset");

    // Use SetSuspendState to put system to sleep (not hibernate)
    // First parameter FALSE = sleep (not hibernate)
    // Second parameter FALSE = force sleep (do not ask applications)
    // Third parameter FALSE = disable wake events
    BOOL result = SetSuspendState(FALSE, FALSE, FALSE);

    if (result) {
        DEBUG_LOG("trigger_system_sleep: successfully initiated sleep");
        return;
    }
    
    DWORD error = GetLastError();
    DEBUG_LOG("trigger_system_sleep: SetSuspendState failed with error %lu", error);
    tray_show_notification(tray, NOTIFY_EVENT_ACTION_FAILED,
        "Sleep Failed", "Failed to put system to sleep. Check power settings.", true);
}

static void trigger_system_shutdown(NoSleepTray* tray, SessionFinishedAction action) {
    DEBUG_LOG("trigger_system_shutdown: shutting down system");

    // Reset execution state to allow Windows to shutdown normally
    SetThreadExecutionState(ES_CONTINUOUS);
    DEBUG_LOG("trigger_system_shutdown: execution state reset");

    // Only the explicit force action closes applications forcibly.
    // Need to adjust privileges first
    HANDLE hToken;
    TOKEN_PRIVILEGES tkp;
    
    // Get a token for this process
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        // Get the LUID for the shutdown privilege
        LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);
        
        tkp.PrivilegeCount = 1;
        tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        
        // Get the shutdown privilege for this process
        AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, (PTOKEN_PRIVILEGES)NULL, 0);
        
        CloseHandle(hToken);
    }
    
    // Try ExitWindowsEx
    BOOL result = ExitWindowsEx(EWX_SHUTDOWN |
                                (action == SESSION_FINISHED_SHUTDOWN ? EWX_FORCE : 0), 0);
    
    if (result) {
        DEBUG_LOG("trigger_system_shutdown: successfully initiated shutdown");
        return;
    }
    
    // First method failed, try alternative method using InitiateSystemShutdown
    DWORD error = GetLastError();
    DEBUG_LOG("trigger_system_shutdown: ExitWindowsEx failed with error %lu", error);
    DEBUG_LOG("trigger_system_shutdown: trying alternative method via InitiateSystemShutdown");
    
    // Try InitiateSystemShutdown
    result = InitiateSystemShutdown(NULL, NULL, 0,
                                    action == SESSION_FINISHED_SHUTDOWN, FALSE);
    
    if (result) {
        DEBUG_LOG("trigger_system_shutdown: InitiateSystemShutdown succeeded");
        return;
    }
    
    // Both methods failed
    DWORD alt_error = GetLastError();
    DEBUG_LOG("trigger_system_shutdown: InitiateSystemShutdown also failed with error %lu", alt_error);
    DEBUG_LOG("trigger_system_shutdown: both shutdown methods failed");
    
    // Show notification to user
    tray_show_notification(tray, NOTIFY_EVENT_ACTION_FAILED,
        "Shutdown Failed", "Failed to shut down system. Check permissions.", true);
}

// Stop joins the delayed worker before reporting cancellation or replacing the
// session. Publish from that worker so a paused duration timer cannot announce
// an action after cleanup, and never call Shell_NotifyIcon under the action lock.
static void tray_announce_delayed_action(NoSleepTray* tray, SessionFinishedAction action) {
    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    bool cancelled = tray->session_action_cancelled || tray->starting_nosleep ||
                     ATOMIC_LOAD_BOOL(&tray->stopping);
    ULONGLONG start_tick64 = tray->start_tick64;
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
    if (cancelled) return;

    ULONGLONG elapsed_seconds = get_elapsed_milliseconds(start_tick64) / 1000;
    int hours = (int)(elapsed_seconds / 3600);
    int minutes = (int)((elapsed_seconds % 3600) / 60);
    int seconds = (int)(elapsed_seconds % 60);
    char duration_message[256];
    if (hours > 0) {
        sprintf(duration_message, "Sleep prevention stopped\nDuration: %dh %dm", hours, minutes);
    } else {
        sprintf(duration_message, "Sleep prevention stopped\nDuration: %dm %ds", minutes, seconds);
    }
    char message[512];
    bool shutdown = action == SESSION_FINISHED_SHUTDOWN ||
                    action == SESSION_FINISHED_SHUTDOWN_GRACEFUL;
    if (action == SESSION_FINISHED_SHUTDOWN) {
        sprintf(message, "%s\nSystem will %s in 60 seconds...\n"
                        "Unsaved work may be lost because applications will be closed forcibly.\n"
                        "Use Cancel shutdown in the tray menu.",
                duration_message, tray_countdown_action_phrase(action));
    } else {
        sprintf(message, "%s\nSystem will %s in 60 seconds...\nUse Cancel %s in the tray menu.",
                duration_message, tray_countdown_action_phrase(action),
                shutdown ? "shutdown" : "sleep");
    }
    tray_show_notification(tray, NOTIFY_EVENT_TIMER_EXPIRED,
                           "Time's up!", message, true);
}

static bool tray_start_delayed_countdown(NoSleepTray* tray,
                                         SessionFinishedAction action,
                                         HANDLE stop_event,
                                         ULONGLONG start_tick64) {
    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    bool cancelled = tray->session_action_cancelled || tray->starting_nosleep ||
                     ATOMIC_LOAD_BOOL(&tray->stopping) ||
                     WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0;
    if (!cancelled) {
        tray->delayed_countdown_starting = true;
    }
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);

    if (cancelled) {
        return false;
    }

    // Keep tray and shell updates outside delayed_action_lock. Stop waits for
    // this reservation to finish before publishing cancellation.
    tray_start_countdown(tray, action, start_tick64);

    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    tray->delayed_countdown_starting = false;
    WakeAllConditionVariable(&tray->stop_condition);
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
    return true;
}

static DWORD WINAPI delayed_sleep_thread(LPVOID lpParam) {
    NoSleepTray* tray = (NoSleepTray*)lpParam;

    DEBUG_LOG("delayed_sleep_thread: waiting 60 seconds before sleep");
    
    tray_announce_delayed_action(tray, SESSION_FINISHED_SLEEP);

    ULONGLONG start_tick64 = GetTickCount64();
    // Dispatch and display share the same 60-second timing target.
    if (!tray_start_delayed_countdown(tray, SESSION_FINISHED_SLEEP,
                                      tray->sleep_stop_event, start_tick64)) {
        tray_reap_delayed_action_handle(tray, &tray->sleep_timer);
        return 0;
    }
    ULONGLONG delay_ms = 60 * 1000;
    
    while (true) {
        // Check for cancellation first
        if (WaitForSingleObject(tray->sleep_stop_event, 0) == WAIT_OBJECT_0) {
            // Cancelled
            DEBUG_LOG("delayed_sleep_thread: cancelled, not sleeping");
            break;
        }
        
        ULONGLONG elapsed_ms = get_elapsed_milliseconds(start_tick64);
        
        if (elapsed_ms >= delay_ms) {
            // Stop the display before deciding atomically whether to trigger sleep.
            tray_stop_countdown(tray);
            AcquireSRWLockExclusive(&tray->delayed_action_lock);
            if (ATOMIC_LOAD_BOOL(&tray->stopping) || WaitForSingleObject(tray->sleep_stop_event, 0) == WAIT_OBJECT_0) {
                DEBUG_LOG("delayed_sleep_thread: cancelled before sleep could start");
                ReleaseSRWLockExclusive(&tray->delayed_action_lock);
                break;
            }

            // Claim dispatch before Stop can cancel it. Keep the handle for
            // cleanup to join, and release the lock before OS power callbacks.
            tray->sleep_action_claimed = true;
            ReleaseSRWLockExclusive(&tray->delayed_action_lock);
            DEBUG_LOG("delayed_sleep_thread: 60 seconds elapsed, triggering sleep");
            trigger_system_sleep(tray);

            // Reap this completed worker before Stop can claim the handle.
            tray_reap_delayed_action_handle(tray, &tray->sleep_timer);
            return 0;
        }
        
        // Wait for 1 second or until cancelled
        DWORD wait_result = WaitForSingleObject(tray->sleep_stop_event, 1000);
        if (wait_result == WAIT_OBJECT_0) {
            // Cancelled
            DEBUG_LOG("delayed_sleep_thread: cancelled, not sleeping");
            break;
        }
    }
    
    // Cancel sleep display (only reached if sleep was cancelled)
    tray_stop_countdown(tray);
    tray_reap_delayed_action_handle(tray, &tray->sleep_timer);

    return 0;
}

static DWORD WINAPI delayed_shutdown_thread(LPVOID lpParam) {
    NoSleepTray* tray = (NoSleepTray*)lpParam;

    DEBUG_LOG("delayed_shutdown_thread: waiting 60 seconds before shutdown");
    
    AcquireSRWLockExclusive(&tray->delayed_action_lock);
    SessionFinishedAction action = tray->shutdown_action;
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);

    tray_announce_delayed_action(tray, action);

    // Start countdown display
    ULONGLONG start_tick64 = GetTickCount64();
    // Dispatch and display share the same 60-second timing target.
    if (!tray_start_delayed_countdown(tray, action,
                                      tray->shutdown_stop_event, start_tick64)) {
        tray_reap_delayed_action_handle(tray, &tray->shutdown_timer);
        return 0;
    }
    ULONGLONG delay_ms = 60 * 1000;
    
    while (true) {
        // Check for cancellation first
        if (WaitForSingleObject(tray->shutdown_stop_event, 0) == WAIT_OBJECT_0) {
            // Cancelled
            DEBUG_LOG("delayed_shutdown_thread: cancelled, not shutting down");
            break;
        }
        
        ULONGLONG elapsed_ms = get_elapsed_milliseconds(start_tick64);
        
        if (elapsed_ms >= delay_ms) {
            // Stop the display before deciding atomically whether to trigger shutdown.
            tray_stop_countdown(tray);
            AcquireSRWLockExclusive(&tray->delayed_action_lock);
            if (ATOMIC_LOAD_BOOL(&tray->stopping) || WaitForSingleObject(tray->shutdown_stop_event, 0) == WAIT_OBJECT_0) {
                DEBUG_LOG("delayed_shutdown_thread: cancelled before shutdown could start");
                ReleaseSRWLockExclusive(&tray->delayed_action_lock);
                break;
            }

            // Claim dispatch before Stop can cancel it. Keep the handle for
            // cleanup to join, and release the lock before OS power callbacks.
            tray->shutdown_action_claimed = true;
            ReleaseSRWLockExclusive(&tray->delayed_action_lock);
            DEBUG_LOG("delayed_shutdown_thread: 60 seconds elapsed, triggering shutdown");
            trigger_system_shutdown(tray, action);

            // Reap this completed worker before Stop can claim the handle.
            tray_reap_delayed_action_handle(tray, &tray->shutdown_timer);
            return 0;
        }
        
        // Wait for 1 second or until cancelled
        DWORD wait_result = WaitForSingleObject(tray->shutdown_stop_event, 1000);
        if (wait_result == WAIT_OBJECT_0) {
            // Cancelled
            DEBUG_LOG("delayed_shutdown_thread: cancelled, not shutting down");
            break;
        }
    }
    
    // Cancel countdown display
    tray_stop_countdown(tray);
    tray_reap_delayed_action_handle(tray, &tray->shutdown_timer);

    return 0;
}

void tray_start_countdown(NoSleepTray* tray, SessionFinishedAction action, ULONGLONG start_tick64) {
    DEBUG_LOG("tray_start_countdown: starting 60-second countdown");
    
    // Stop any existing countdown
    tray_stop_countdown(tray);
    
    // Initialize countdown state
    tray->countdown_action = action;
    tray->countdown_start_tick64 = start_tick64;
    ULONGLONG elapsed_ms = get_elapsed_milliseconds(start_tick64);
    int remaining_seconds = elapsed_ms >= 60000 ? 0 :
        tray_countdown_display_seconds(60000 - elapsed_ms);
    ATOMIC_STORE_BOOL(&tray->delayed_sleep_countdown_active, true);
    ATOMIC_STORE_INT(&tray->countdown_seconds, remaining_seconds);
    ATOMIC_STORE_BOOL(&tray->countdown_blink_state, true);
    ATOMIC_STORE_BOOL(&tray->countdown_stopping, false);
    ResetEvent(tray->countdown_stop_event);
    
    // Create countdown thread
    tray->countdown_timer_thread = CreateThread(
        NULL, 0, countdown_thread, tray, 0, NULL
    );
    if (!tray->countdown_timer_thread) {
        ATOMIC_STORE_BOOL(&tray->delayed_sleep_countdown_active, false);
        DEBUG_LOG("tray_start_countdown: failed to create countdown thread");
        char message[128];
        snprintf(message, sizeof(message),
                 "System will %s in 60 seconds, but the countdown display could not be started.",
                 (action == SESSION_FINISHED_SHUTDOWN || action == SESSION_FINISHED_SHUTDOWN_GRACEFUL) ? "shut down" : "sleep");
        tray_show_notification(tray, NOTIFY_EVENT_ERROR,
                               "Countdown Display Unavailable", message, true);
    } else {
        DEBUG_LOG("tray_start_countdown: countdown thread created");
    }
    
    // Update icon immediately to show initial countdown state
    tray_update_icon(tray);
    
    // Update stop menu item text
    tray_update_stop_menu_item(tray);
}

void tray_stop_countdown(NoSleepTray* tray) {
    DEBUG_LOG("tray_stop_countdown: stopping countdown, delayed_sleep_countdown_active=%s, countdown_stopping=%s",
            ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active) ? "true" : "false",
            ATOMIC_LOAD_BOOL(&tray->countdown_stopping) ? "true" : "false");
    
    // Claim cleanup before inspecting the shared countdown thread handle.
    if (ATOMIC_EXCHANGE_BOOL(&tray->countdown_stopping, true)) {
        DEBUG_LOG("tray_stop_countdown: already stopping, returning");
        return;
    }
    
    if (!tray_countdown_has_work(ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active),
                                 tray->countdown_timer_thread != NULL)) {
        DEBUG_LOG("tray_stop_countdown: no active countdown or countdown thread, returning");
        ATOMIC_STORE_BOOL(&tray->countdown_stopping, false);
        return;
    }
    
    DEBUG_LOG("tray_stop_countdown: setting delayed_sleep_countdown_active=false");
    // Signal countdown thread to stop first
    if (tray->countdown_stop_event) {
        SetEvent(tray->countdown_stop_event);
    }
    
    // Then set flag to false to ensure thread sees the event
    ATOMIC_STORE_BOOL(&tray->delayed_sleep_countdown_active, false);
    // Ensure memory visibility across threads
    MEMORY_BARRIER();
    
    // Wait for countdown thread to finish
    if (tray->countdown_timer_thread) {
        // Use INFINITE wait to ensure thread exits completely
        WaitForSingleObject(tray->countdown_timer_thread, INFINITE);
        CloseHandle(tray->countdown_timer_thread);
        tray->countdown_timer_thread = NULL;
    }
    
    // Reset event for future use
    if (tray->countdown_stop_event) {
        ResetEvent(tray->countdown_stop_event);
    }
    
    // Update icon to remove countdown display
    tray_update_icon(tray);
    
    // Update stop menu item text
    tray_update_stop_menu_item(tray);
    
    // Reset stopping flag
    ATOMIC_STORE_BOOL(&tray->countdown_stopping, false);
}

DWORD WINAPI countdown_thread(LPVOID lpParam) {
    NoSleepTray* tray = (NoSleepTray*)lpParam;
    
    DEBUG_LOG("countdown_thread: started");
    
    ULONGLONG start_tick64 = tray->countdown_start_tick64;
    ULONGLONG total_duration_ms = 60 * 1000;
    
    while (ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active)) {
        ULONGLONG elapsed_ms = get_elapsed_milliseconds(start_tick64);
        
        // Check if countdown finished
        if (elapsed_ms >= total_duration_ms) {
            DEBUG_LOG("countdown_thread: countdown finished");
            ATOMIC_STORE_INT(&tray->countdown_seconds, 0);
            break;
        }
        
        // Calculate remaining seconds
        ULONGLONG remaining_ms = total_duration_ms - elapsed_ms;
        int remaining_seconds = tray_countdown_display_seconds(remaining_ms);
        
        // Calculate blink state based on elapsed milliseconds
        // We want to blink every 500ms (0.5 seconds)
        bool blink_state = ((elapsed_ms / 500) % 2) == 0; // True for first 500ms of each second
        
        // Update atomic variables
        ATOMIC_STORE_INT(&tray->countdown_seconds, remaining_seconds);
        ATOMIC_STORE_BOOL(&tray->countdown_blink_state, blink_state);
        
        DEBUG_LOG("countdown_thread: seconds=%d, blink=%s, elapsed_ms=%llu", 
                remaining_seconds, blink_state ? "show" : "hide", elapsed_ms);
        
        // Update icon
        tray_update_icon(tray);
        
        // Check for stop event
        if (WaitForSingleObject(tray->countdown_stop_event, 0) == WAIT_OBJECT_0) {
            // Stop event signaled
            DEBUG_LOG("countdown_thread: stop event signaled, delayed_sleep_countdown_active=%s, countdown_seconds=%d",
                    ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active) ? "true" : "false", remaining_seconds);
            break;
        }
        
        // Wait for a short time (100ms) or until stop event
        DWORD wait_result = WaitForSingleObject(tray->countdown_stop_event, 100);
        if (wait_result == WAIT_OBJECT_0) {
            // Stop event signaled
            DEBUG_LOG("countdown_thread: stop event signaled during wait");
            break;
        }
    }
    
    ATOMIC_STORE_BOOL(&tray->delayed_sleep_countdown_active, false);
    // Ensure memory visibility across threads
    MEMORY_BARRIER();
    
    // Update icon and stop menu item when countdown finishes naturally
    tray_update_icon(tray);
    tray_update_stop_menu_item(tray);
    
    DEBUG_LOG("countdown_thread: exiting, delayed_sleep_countdown_active=%s",
            ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active) ? "true" : "false");
    
    return 0;
}

void tray_update_icon(NoSleepTray* tray) {
    int remaining_minutes = -1;
    if (!tray || !tray->hwnd) return;

    AcquireSRWLockExclusive(&tray->tray_icon_lock);
    
    // Take local snapshots of atomic variables for consistency
    bool delayed_sleep_countdown_active = ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active);
    int countdown_seconds = ATOMIC_LOAD_INT(&tray->countdown_seconds);
    bool countdown_blink_state = ATOMIC_LOAD_BOOL(&tray->countdown_blink_state);
    bool is_running = ATOMIC_LOAD_BOOL(&tray->is_running);
    
    DEBUG_LOG("tray_update_icon: delayed_sleep_countdown_active=%s",
            delayed_sleep_countdown_active ? "true" : "false");
    
    // Handle delayed sleep countdown display
    if (delayed_sleep_countdown_active) {
        DEBUG_LOG("tray_update_icon: countdown active, seconds=%d, blink=%s",
                countdown_seconds, countdown_blink_state ? "show" : "hide");
        
        AcquireSRWLockExclusive(&tray->countdown_icon_cache_lock);
        tray->nid.hIcon = tray_countdown_display_icon(
            tray->hIconNumbered, countdown_seconds, countdown_blink_state,
            tray->hIconCountdownBlank, tray->hIconDefault, create_numbered_icon);
        ReleaseSRWLockExclusive(&tray->countdown_icon_cache_lock);
        
        // Update tooltip with remaining seconds
        char tip[128];
        tray_format_countdown_tooltip(tip, sizeof(tip),
                                      tray->countdown_action,
                                      countdown_seconds);
        strcpy(tray->nid.szTip, tip);
        
        tray->nid.uFlags = NIF_ICON | NIF_TIP;
        BOOL success = FALSE;
        int retry_count = 0;
        const int max_retries = 3;
        while (!success && retry_count < max_retries) {
            success = Shell_NotifyIcon(NIM_MODIFY, &tray->nid);
            if (!success && retry_count < max_retries - 1) {
                Sleep(50); // Small delay before retry
            }
            retry_count++;
        }
        DEBUG_LOG("tray_update_icon: Shell_NotifyIcon (countdown) %s after %d attempts", 
                success ? "succeeded" : "failed", retry_count);
        ReleaseSRWLockExclusive(&tray->tray_icon_lock);
        return;
    }
    
    if (is_running) {
        if (tray->duration_minutes > 0) {
            // Calculate remaining minutes
            ULONGLONG elapsed_seconds = get_elapsed_milliseconds(tray->start_tick64) / 1000;
            ULONGLONG total_seconds = tray->duration_minutes * 60ULL;
            ULONGLONG remaining_seconds = total_seconds > elapsed_seconds ? total_seconds - elapsed_seconds : 0;
            remaining_minutes = (int)(remaining_seconds / 60);
            int display_number = remaining_minutes > 0 ? remaining_minutes : (int)remaining_seconds;
            
            // Update icon with number
            if (display_number != tray->current_number) {
                DEBUG_LOG("tray_update_icon: display_number=%d current_number=%d remaining_minutes=%d remaining_seconds=%llu", display_number, tray->current_number, remaining_minutes, remaining_seconds);
                // Release the prior icon according to its ownership.
                if (tray->hIconCurrentNumbered) {
                    tray_release_numbered_icon(&tray->hIconCurrentNumbered,
                                               tray->current_number,
                                               tray->hIconActive, DestroyIcon);
                }
                
                // Get or create numbered icon
                HICON numbered_icon = NULL;
                if (display_number >= 0 && display_number < 60) {
                    // Use cached icon
                    if (tray->hIconNumbered[display_number] == NULL) {
                        tray->hIconNumbered[display_number] = create_numbered_icon(display_number);
                    }
                    numbered_icon = tray->hIconNumbered[display_number];
                } else {
                    // Create new icon for numbers >=60
                    numbered_icon = create_numbered_icon(display_number);
                }
                
                // Fallback to active icon if numbered icon creation failed
                if (numbered_icon) {
                    tray->hIconCurrentNumbered = numbered_icon;
                } else {
                    DEBUG_LOG("tray_update_icon: numbered icon creation failed, using active icon");
                    tray->hIconCurrentNumbered = tray->hIconActive;
                }
                tray->current_number = display_number;
            }
            
            tray->nid.hIcon = tray->hIconCurrentNumbered ? tray->hIconCurrentNumbered : tray->hIconActive;
            
            // Update tooltip
            char tip[128];
            DEBUG_LOG("tray_update_icon: remaining_seconds=%llu remaining_minutes=%d", remaining_seconds, remaining_minutes);
            if (remaining_seconds >= 3600) {
                int hours = (int)(remaining_seconds / 3600);
                int minutes = (int)((remaining_seconds % 3600) / 60);
                sprintf(tip, "nosleep - %dh %dm remaining", hours, minutes);
            } else if (remaining_seconds >= 60) {
                int minutes = (int)(remaining_seconds / 60);
                int seconds = (int)(remaining_seconds % 60);
                sprintf(tip, "nosleep - %dm %ds remaining", minutes, seconds);
            } else {
                sprintf(tip, "nosleep - %ds remaining", (int)remaining_seconds);
            }
            strcpy(tray->nid.szTip, tip);
            DEBUG_LOG("tray_update_icon: tooltip='%s'", tip);
        } else {
            // Indefinite
            tray_ensure_indefinite_icon(&tray->hIconCurrentNumbered, &tray->current_number,
                                        tray->hIconActive,
                                        create_numbered_icon, DestroyIcon);
            
            tray->nid.hIcon = tray->hIconCurrentNumbered ? tray->hIconCurrentNumbered : tray->hIconActive;
            strcpy(tray->nid.szTip, "nosleep - Active (indefinite)");
        }
    } else {
        // Not running
        tray->nid.hIcon = tray->hIconDefault;
        strcpy(tray->nid.szTip, "nosleep - System sleep prevention");
    }
    
    tray->nid.uFlags = NIF_ICON | NIF_TIP;
    DEBUG_LOG("tray_update_icon: setting hIcon=%p, remaining_minutes=%d", tray->nid.hIcon, remaining_minutes);
    BOOL success = FALSE;
    int retry_count = 0;
    const int max_retries = 3;
    while (!success && retry_count < max_retries) {
        success = Shell_NotifyIcon(NIM_MODIFY, &tray->nid);
        if (!success && retry_count < max_retries - 1) {
            Sleep(50); // Small delay before retry
        }
        retry_count++;
    }
    DEBUG_LOG("tray_update_icon: Shell_NotifyIcon %s after %d attempts", success ? "succeeded" : "failed", retry_count);
    ReleaseSRWLockExclusive(&tray->tray_icon_lock);
}

void tray_update_stop_menu_item(NoSleepTray* tray) {
    if (!tray || !tray->hmenu) return;
    
    // Get current state
    bool delayed_sleep_countdown_active = ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active);
    bool is_running = ATOMIC_LOAD_BOOL(&tray->is_running);
    
    // Determine menu text based on state
    const char* stop_text = NULL;
    if (delayed_sleep_countdown_active) {
        // Check if it's shutdown or sleep countdown
        if (tray->countdown_action == SESSION_FINISHED_SHUTDOWN ||
            tray->countdown_action == SESSION_FINISHED_SHUTDOWN_GRACEFUL) {
            stop_text = "Cancel shutdown";
        } else {
            stop_text = "Cancel sleep";
        }
    } else if (is_running) {
        stop_text = "Stop nosleep session";
    } else {
        stop_text = "Stop"; // Default when not active
    }
    
    // Update the menu item text
    MENUITEMINFO mii = {0};
    mii.cbSize = sizeof(MENUITEMINFO);
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = (LPSTR)stop_text;
    
    SetMenuItemInfo(tray->hmenu, IDM_STOP, FALSE, &mii);
    
    DEBUG_LOG("tray_update_stop_menu_item: updated to '%s' (countdown_active=%s, is_running=%s, session_action=%d)",
            stop_text, delayed_sleep_countdown_active ? "true" : "false", is_running ? "true" : "false", tray->session_finished_action);
}

void tray_update_session_finished_menu(NoSleepTray* tray) {
    if (!tray || !tray->hmenu) return;
    
    DEBUG_LOG("tray_update_session_finished_menu: action=%d", tray->session_finished_action);
    
    // Get the When finished submenu (position 0 in the main menu)
    HMENU hSubMenu = GetSubMenu(tray->hmenu, 0);
    if (!hSubMenu) return;
    
    // Update checkmarks for all three radio items in the submenu
    CheckMenuItem(hSubMenu, IDM_SESSION_FINISHED_NONE, 
                  MF_BYCOMMAND | (tray->session_finished_action == SESSION_FINISHED_NONE ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_SESSION_FINISHED_SHUTDOWN, 
                  MF_BYCOMMAND | (tray->session_finished_action == SESSION_FINISHED_SHUTDOWN ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_SESSION_FINISHED_SHUTDOWN_GRACEFUL,
                  MF_BYCOMMAND | (tray->session_finished_action == SESSION_FINISHED_SHUTDOWN_GRACEFUL ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_SESSION_FINISHED_SLEEP, 
                  MF_BYCOMMAND | (tray->session_finished_action == SESSION_FINISHED_SLEEP ? MF_CHECKED : MF_UNCHECKED));
    
    // Update main menu item text to show current selection
    char finished_text[64];
    switch (tray->session_finished_action) {
        case SESSION_FINISHED_NONE:
            snprintf(finished_text, sizeof(finished_text), "When finished (None)");
            break;
        case SESSION_FINISHED_SHUTDOWN:
            snprintf(finished_text, sizeof(finished_text), "When finished (Shutdown: force)");
            break;
        case SESSION_FINISHED_SHUTDOWN_GRACEFUL:
            snprintf(finished_text, sizeof(finished_text), "When finished (Shutdown: graceful)");
            break;
        case SESSION_FINISHED_SLEEP:
            snprintf(finished_text, sizeof(finished_text), "When finished (Sleep)");
            break;
        default:
            snprintf(finished_text, sizeof(finished_text), "When finished");
            break;
    }
    
    MENUITEMINFO mii = {0};
    mii.cbSize = sizeof(MENUITEMINFO);
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = finished_text;
    
    // Update the main menu item (position 0 = When finished submenu)
    SetMenuItemInfo(tray->hmenu, 0, TRUE, &mii);
    
    DEBUG_LOG("tray_update_session_finished_menu: updated to '%s'", finished_text);
}

// PATH registry values use UTF-16, independently of the application's ANSI UI.
static wchar_t* get_exe_path_w(void) {
    size_t capacity = MAX_PATH;
    wchar_t* path = (wchar_t*)malloc(capacity * sizeof(wchar_t));
    if (!path) return NULL;

    for (;;) {
        DWORD len = GetModuleFileNameW(NULL, path, (DWORD)capacity);
        if (len == 0) {
            free(path);
            return NULL;
        }
        if ((size_t)len < capacity) return path;

        if (capacity > (size_t)MAXDWORD / 2 ||
            capacity > ((size_t)-1 / sizeof(wchar_t)) / 2) {
            free(path);
            return NULL;
        }

        size_t new_capacity = capacity * 2;
        wchar_t* new_path = (wchar_t*)realloc(
            path, new_capacity * sizeof(wchar_t));
        if (!new_path) {
            free(path);
            return NULL;
        }
        path = new_path;
        capacity = new_capacity;
    }
}

// Compare bounded UTF-16 PATH segments using Windows ordinal case folding.
static bool path_segment_equal(const wchar_t* a, const wchar_t* b, size_t n) {
    // A preserved root separator may use either slash form in PATH.
    if (n > 0 && (a[n - 1] == L'\\' || a[n - 1] == L'/') &&
        (b[n - 1] == L'\\' || b[n - 1] == L'/')) {
        n--;
    }
    if (n == 0) return true;
    if (n > INT_MAX) return false;
    // A zero API result indicates failure and must never count as a match.
    return CompareStringOrdinal(a, (int)n, b, (int)n, TRUE) == CSTR_EQUAL;
}

// Ignore optional trailing directory separators during PATH comparisons, but
// keep drive-root separators so C:\\ does not become the drive-relative C:.
static size_t path_segment_comparison_length(const wchar_t* path, size_t len) {
    while (len > 0 && (path[len - 1] == L'\\' || path[len - 1] == L'/')) {
        bool drive_root = len == 3 && path[1] == L':' &&
            ((path[0] >= L'A' && path[0] <= L'Z') ||
             (path[0] >= L'a' && path[0] <= L'z'));
        bool extended_drive_root = len == 7 && path[0] == L'\\' &&
            path[1] == L'\\' && path[2] == L'?' && path[3] == L'\\' &&
            path[5] == L':' &&
            ((path[4] >= L'A' && path[4] <= L'Z') ||
             (path[4] >= L'a' && path[4] <= L'z'));
        if (drive_root || extended_drive_root || len <= 2) break;
        len--;
    }
    return len;
}

// Get the directory containing the executable
static wchar_t* get_exe_dir(void) {
    wchar_t* path = get_exe_path_w();
    if (!path) return NULL;
    
    // Find last backslash and terminate there to get directory
    wchar_t* last_backslash = wcsrchr(path, '\\');
    if (last_backslash) {
        // Keep the separator when the executable is directly under a drive root.
        if ((last_backslash == path + 2 && path[1] == ':') ||
            (last_backslash == path + 6 &&
             wcsncmp(path, L"\\\\?\\", 4) == 0 && path[5] == ':')) {
            last_backslash[1] = '\0';
        } else {
            *last_backslash = '\0';
        }
    }
    return path; // Caller must free()
}

// Add nosleep directory to user PATH in registry
static bool add_app_to_path(void) {
    wchar_t* dir = get_exe_dir();
    if (!dir) return false;
    // PATH uses semicolons as delimiters and cannot escape them in a directory.
    if (wcschr(dir, L';')) {
        free(dir);
        return false;
    }
    
    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Environment", 0, KEY_READ | KEY_WRITE, &hKey);
    if (result == ERROR_FILE_NOT_FOUND) {
        result = RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Environment", 0, NULL, REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_WRITE, NULL, &hKey, NULL);
    }
    if (result != ERROR_SUCCESS) {
        free(dir);
        return false;
    }
    
    // Read current PATH
    DWORD path_size = 0;
    DWORD path_type = REG_EXPAND_SZ;
    result = RegQueryValueExW(hKey, L"Path", NULL, &path_type, NULL, &path_size);
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
        RegCloseKey(hKey);
        free(dir);
        return false;
    }
    
    if (result == ERROR_FILE_NOT_FOUND) path_type = REG_EXPAND_SZ;

    if (path_size == 0) {
        // REG_EXPAND_SZ cannot escape literal percent signs in an install path.
        if (path_type == REG_EXPAND_SZ && wcschr(dir, L'%')) {
            RegCloseKey(hKey);
            free(dir);
            return false;
        }
        // PATH is empty or doesn't exist, just set it to our directory
        result = RegSetValueExW(hKey, L"Path", 0, path_type,
            (LPBYTE)dir, (DWORD)((wcslen(dir) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
        free(dir);
        
        if (result != ERROR_SUCCESS) return false;
        
        // Notify the system about the environment change
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
            (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
        return true;
    }
    
    // Allocate buffer for current path + new dir + semicolon
    DWORD buf_size = path_size + (DWORD)((wcslen(dir) + 2) * sizeof(wchar_t)); // +2 for ';' and '\0'
    wchar_t* new_path = (wchar_t*)malloc(buf_size);
    if (!new_path) {
        RegCloseKey(hKey);
        free(dir);
        return false;
    }
    new_path[0] = '\0';
    
    // Read PATH data with retry for TOCTOU safety (PATH may have changed between calls)
    DWORD actual_size = path_size;
    result = RegQueryValueExW(hKey, L"Path", NULL, &path_type, (LPBYTE)new_path, &actual_size);
    if (result == ERROR_MORE_DATA) {
        // PATH grew between calls, reallocate and retry
        buf_size = actual_size + (DWORD)((wcslen(dir) + 2) * sizeof(wchar_t));
        wchar_t* realloc_path = (wchar_t*)realloc(new_path, buf_size);
        if (!realloc_path) {
            RegCloseKey(hKey);
            free(new_path);
            free(dir);
            return false;
        }
        new_path = realloc_path;
        result = RegQueryValueExW(hKey, L"Path", NULL, &path_type, (LPBYTE)new_path, &actual_size);
    }
    if (result != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        free(new_path);
        free(dir);
        return false;
    }
    // Use the type from the final read; it may have changed since the size query.
    // Refuse an unsafe addition rather than expanding or rewriting existing PATH.
    if (path_type == REG_EXPAND_SZ && wcschr(dir, L'%')) {
        RegCloseKey(hKey);
        free(new_path);
        free(dir);
        return false;
    }
    // Ensure null-terminated (actual_size is bytes including null in registry strings)
    if (actual_size >= buf_size || actual_size % sizeof(wchar_t) != 0) {
        RegCloseKey(hKey);
        free(new_path);
        free(dir);
        return false;
    }
    new_path[actual_size / sizeof(wchar_t)] = '\0';
        
        // Check if dir is already in PATH (case-insensitive)
        // Remove trailing spaces and semicolons for clean comparison
        size_t dir_len = path_segment_comparison_length(dir, wcslen(dir));
        wchar_t* p = new_path;
        bool already_in_path = false;
        while (*p && !already_in_path) {
            // Skip leading spaces
            while (*p == ' ') p++;
            if (*p == '\0') break;
            
            // Find next semicolon or end
            wchar_t* next = wcschr(p, ';');
            size_t seg_len = next ? (size_t)(next - p) : wcslen(p);
            
            // Trim trailing spaces
            while (seg_len > 0 && p[seg_len - 1] == ' ') seg_len--;

            seg_len = path_segment_comparison_length(p, seg_len);
            
            if (seg_len == dir_len && path_segment_equal(p, dir, dir_len)) {
                already_in_path = true;
                break;
            }
            
            if (next) {
                p = next + 1;
            } else {
                break;
            }
        }
        
        if (already_in_path) {
            free(new_path);
            RegCloseKey(hKey);
            free(dir);
            return true; // Already in PATH, consider it success
        }
        
        // Append semicolon if existing path doesn't end with one
        size_t existing_len = wcslen(new_path);
        if (existing_len > 0 && new_path[existing_len - 1] != ';') {
            wcscat(new_path, L";");
        }
    
    // Append new directory
    wcscat(new_path, dir);
    
    // Write back to registry
    result = RegSetValueExW(hKey, L"Path", 0, path_type,
        (LPBYTE)new_path, (DWORD)((wcslen(new_path) + 1) * sizeof(wchar_t)));
    
    RegCloseKey(hKey);
    free(new_path);
    free(dir);
    
    if (result != ERROR_SUCCESS) return false;
    
    // Notify the system about the environment change
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
        (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
    
    return true;
}

// Remove nosleep directory from user PATH in registry
static bool remove_app_from_path(void) {
    wchar_t* dir = get_exe_dir();
    if (!dir) return false;
    
    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Environment", 0, KEY_READ | KEY_WRITE, &hKey);
    if (result != ERROR_SUCCESS) {
        free(dir);
        return result == ERROR_FILE_NOT_FOUND;
    }
    
    // Read current PATH
    DWORD path_size = 0;
    DWORD path_type = REG_EXPAND_SZ;
    result = RegQueryValueExW(hKey, L"Path", NULL, &path_type, NULL, &path_size);
    if (result == ERROR_FILE_NOT_FOUND || (result == ERROR_SUCCESS && path_size == 0)) {
        RegCloseKey(hKey);
        free(dir);
        return true;
    }
    if (result != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        free(dir);
        return false;
    }
    
    wchar_t* current_path = (wchar_t*)malloc(path_size + sizeof(wchar_t));
    if (!current_path) {
        RegCloseKey(hKey);
        free(dir);
        return false;
    }
    
    // Read PATH data with retry for TOCTOU safety (PATH may have changed between calls)
    DWORD actual_size = path_size;
    DWORD buf_size = path_size;
    result = RegQueryValueExW(hKey, L"Path", NULL, &path_type, (LPBYTE)current_path, &actual_size);
    if (result == ERROR_MORE_DATA) {
        // PATH grew between calls, reallocate and retry
        buf_size = actual_size;
        wchar_t* realloc_path = (wchar_t*)realloc(current_path, buf_size + sizeof(wchar_t));
        if (!realloc_path) {
            free(current_path);
            RegCloseKey(hKey);
            free(dir);
            return false;
        }
        current_path = realloc_path;
        result = RegQueryValueExW(hKey, L"Path", NULL, &path_type, (LPBYTE)current_path, &actual_size);
    }
    if (result == ERROR_FILE_NOT_FOUND) {
        free(current_path);
        RegCloseKey(hKey);
        free(dir);
        return true;
    }
    if (result != ERROR_SUCCESS) {
        free(current_path);
        RegCloseKey(hKey);
        free(dir);
        return false;
    }
    // Ensure null-terminated (registry strings include null terminator in count)
    if (actual_size > buf_size || actual_size % sizeof(wchar_t) != 0) {
        free(current_path);
        RegCloseKey(hKey);
        free(dir);
        return false;
    }
    current_path[actual_size / sizeof(wchar_t)] = '\0';
    
    size_t dir_len = path_segment_comparison_length(dir, wcslen(dir));
    bool found = false;
    
    // Build new path by removing all occurrences of dir (handle duplicates)
    // Use wcslen(current_path) since the actual data might be larger than path_size from first query
    size_t new_size = wcslen(current_path) + 1;
    wchar_t* new_path = (wchar_t*)malloc(new_size * sizeof(wchar_t));
    if (!new_path) {
        free(current_path);
        RegCloseKey(hKey);
        free(dir);
        return false;
    }
    new_path[0] = '\0';
    
    bool kept_segment = false;
    wchar_t* p = current_path;
    for (;;) {
        // Find next semicolon or end (without modifying p yet)
        wchar_t* seg_start = p;
        wchar_t* next = wcschr(p, ';');
        size_t seg_len = next ? (size_t)(next - p) : wcslen(p);
        
        // Trim leading spaces for comparison only
        wchar_t* compare_start = p;
        while (*compare_start == ' ' && compare_start < p + seg_len) compare_start++;
        size_t remaining = seg_len - (size_t)(compare_start - p);
        
        // Trim trailing spaces for comparison
        size_t trimmed_len = remaining;
        while (trimmed_len > 0 && compare_start[trimmed_len - 1] == ' ') trimmed_len--;

        trimmed_len = path_segment_comparison_length(compare_start, trimmed_len);
        
        // Check if this segment matches our directory
        bool is_match = (trimmed_len == dir_len && path_segment_equal(compare_start, dir, dir_len));
        
        if (!is_match) {
            // Keep this segment with original formatting
            if (kept_segment) {
                wcscat(new_path, L";");
            }
            wcsncat(new_path, seg_start, seg_len);
            kept_segment = true;
        } else {
            found = true;
        }
        
        if (next) {
            p = next + 1;
        } else {
            break;
        }
    }
    
    if (!found) {
        // Not in PATH, nothing to remove
        free(new_path);
        free(current_path);
        RegCloseKey(hKey);
        free(dir);
        return true;
    }
    
    // Write back to registry
    result = RegSetValueExW(hKey, L"Path", 0, path_type,
        (LPBYTE)new_path, (DWORD)((wcslen(new_path) + 1) * sizeof(wchar_t)));
    
    RegCloseKey(hKey);
    free(new_path);
    free(current_path);
    free(dir);
    
    if (result != ERROR_SUCCESS) return false;
    
    // Notify the system about the environment change
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
        (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
    
    return true;
}

static bool apply_path_preference(bool add_to_path) {
    return add_to_path ? add_app_to_path() : remove_app_from_path();
}

// Check if nosleep is registered to run at startup
static bool is_startup_enabled(void) {
    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) return false;

    wchar_t* exe_path = get_exe_path_w();
    if (!exe_path) {
        RegCloseKey(hKey);
        return false;
    }

    // Build expected registry value: "exe_path" --startup
    // Need to account for possible quotes around exe path
    size_t exe_len = wcslen(exe_path);
    size_t expected_len = exe_len + 13;
    wchar_t* expected = (wchar_t*)malloc(expected_len * sizeof(wchar_t));
    if (!expected) {
        free(exe_path);
        RegCloseKey(hKey);
        return false;
    }
    swprintf(expected, expected_len, L"\"%ls\" --startup", exe_path);

    wchar_t* reg_value = (wchar_t*)malloc(expected_len * sizeof(wchar_t));
    if (!reg_value) {
        free(exe_path);
        free(expected);
        RegCloseKey(hKey);
        return false;
    }
    DWORD value_size = (DWORD)(expected_len * sizeof(wchar_t));
    DWORD value_type = 0;
    result = RegQueryValueExW(hKey, L"nosleep", NULL, &value_type,
                             (LPBYTE)reg_value, &value_size);
    RegCloseKey(hKey);

    bool enabled = (result == ERROR_SUCCESS && value_type == REG_SZ &&
        value_size >= sizeof(wchar_t) &&
        value_size <= expected_len * sizeof(wchar_t) &&
        value_size % sizeof(wchar_t) == 0 &&
        reg_value[value_size / sizeof(wchar_t) - 1] == L'\0' &&
        wcscmp(reg_value, expected) == 0);
    free(exe_path);
    free(expected);
    free(reg_value);
    return enabled;
}

static bool set_startup_registry(bool enable) {
    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_WRITE, &hKey);
    if (enable && result == ERROR_FILE_NOT_FOUND) {
        result = RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    }
    if (result != ERROR_SUCCESS) {
        return !enable && result == ERROR_FILE_NOT_FOUND;
    }

    if (enable) {
        wchar_t* exe_path = get_exe_path_w();
        if (!exe_path) {
            RegCloseKey(hKey);
            return false;
        }

        // Store as: "exe_path" --startup
        size_t value_len = wcslen(exe_path) + 13;
        wchar_t* value = (wchar_t*)malloc(value_len * sizeof(wchar_t));
        if (!value) {
            free(exe_path);
            RegCloseKey(hKey);
            return false;
        }
        swprintf(value, value_len, L"\"%ls\" --startup", exe_path);
        result = RegSetValueExW(hKey, L"nosleep", 0, REG_SZ,
                               (LPBYTE)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
        free(value);
        free(exe_path);
    } else {
        result = RegDeleteValueW(hKey, L"nosleep");
    }

    RegCloseKey(hKey);
    return result == ERROR_SUCCESS || (!enable && result == ERROR_FILE_NOT_FOUND);
}

#define SETTINGS_REG_KEY "Software\\nosleep\\settings"

static void settings_read_dword(HKEY hKey, const char* name, DWORD* value, DWORD default_value) {
    DWORD size = sizeof(DWORD);
    DWORD data = 0;
    DWORD type = 0;
    LONG result = hKey ? RegQueryValueEx(hKey, name, NULL, &type, (LPBYTE)&data, &size)
                       : ERROR_FILE_NOT_FOUND;
    if (result == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(DWORD)) {
        *value = data;
    } else {
        *value = default_value;
    }
}

static int settings_load_custom_duration(void) {
    HKEY hKey;
    LONG result = RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY,
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ, NULL, &hKey, NULL);
    if (result != ERROR_SUCCESS) return 30;

    DWORD value;
    settings_read_dword(hKey, "custom_duration_minutes", &value, 30);
    RegCloseKey(hKey);
    return value >= 1 && value <= 1440 ? (int)value : 30;
}

static void settings_save_custom_duration(int duration) {
    if (duration < 1 || duration > 1440) return;

    HKEY hKey;
    LONG result = RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY,
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (result != ERROR_SUCCESS) return;

    DWORD value = (DWORD)duration;
    RegSetValueEx(hKey, "custom_duration_minutes", 0, REG_DWORD,
                  (LPBYTE)&value, sizeof(value));
    RegCloseKey(hKey);
}

void tray_load_settings(NoSleepTray* tray) {
    if (!tray) return;
    HKEY hKey;
    LONG result = RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY,
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ, NULL, &hKey, NULL);
    // Apply the per-setting defaults when the settings key is unavailable.
    if (result != ERROR_SUCCESS) hKey = NULL;

    DWORD val;
    settings_read_dword(hKey, "prevent_display", &val, 0);
    tray->prevent_display = (val != 0);

    settings_read_dword(hKey, "away_mode", &val, 0);
    tray->away_mode = (val != 0);

    settings_read_dword(hKey, "verbose_logging", &val, 0);
    tray->verbose = (val != 0);

    settings_read_dword(hKey, "check_updates_on_startup", &val, 1);
    tray->check_updates_on_startup = (val != 0);

    settings_read_dword(hKey, "auto_check_interval", &val, 1);
    tray->auto_check_interval = (int)val;

    settings_read_dword(hKey, "notification_mode", &val, (DWORD)NOTIFY_ALL);
    tray->notification_mode = (int)val;

    settings_read_dword(hKey, "add_to_path", &val, 0);
    tray->add_to_path = (val != 0);

    settings_read_dword(hKey, "session_finished_action", &val, (DWORD)SESSION_FINISHED_NONE);
    tray->session_finished_action = val <= SESSION_FINISHED_SHUTDOWN_GRACEFUL
        ? (SessionFinishedAction)val : SESSION_FINISHED_NONE;

    if (hKey) RegCloseKey(hKey);
}

bool tray_save_settings(NoSleepTray* tray) {
    if (!tray) return false;
    HKEY hKey;
    LONG result = RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY,
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (result != ERROR_SUCCESS) {
        // Notification groups use their own key, so attempt their save even
        // when the general settings key is unavailable.
        notify_groups_save(&tray->notify_groups);
        return false;
    }

    bool success = true;

    DWORD val = tray->prevent_display ? 1 : 0;
    if (RegSetValueEx(hKey, "prevent_display", 0, REG_DWORD, (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
        success = false;
    }

    val = tray->away_mode ? 1 : 0;
    if (RegSetValueEx(hKey, "away_mode", 0, REG_DWORD, (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
        success = false;
    }

    val = tray->verbose ? 1 : 0;
    if (RegSetValueEx(hKey, "verbose_logging", 0, REG_DWORD, (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
        success = false;
    }

    val = tray->check_updates_on_startup ? 1 : 0;
    if (RegSetValueEx(hKey, "check_updates_on_startup", 0, REG_DWORD, (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
        success = false;
    }

    val = (DWORD)tray->auto_check_interval;
    if (RegSetValueEx(hKey, "auto_check_interval", 0, REG_DWORD, (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
        success = false;
    }

    val = (DWORD)tray->notification_mode;
    if (RegSetValueEx(hKey, "notification_mode", 0, REG_DWORD, (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
        success = false;
    }

    val = tray->add_to_path ? 1 : 0;
    if (RegSetValueEx(hKey, "add_to_path", 0, REG_DWORD, (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
        success = false;
    }

    val = (DWORD)tray->session_finished_action;
    if (RegSetValueEx(hKey, "session_finished_action", 0, REG_DWORD, (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
        success = false;
    }

    RegCloseKey(hKey);

    // Save notification groups separately
    if (!notify_groups_save(&tray->notify_groups)) {
        success = false;
    }
    return success;
}

static bool tray_save_settings_with_warning(HWND hwnd, NoSleepTray* tray) {
    bool success = tray_save_settings(tray);
    if (!success) {
        MessageBox(hwnd,
            "Some settings could not be saved. The changes may be lost when NoSleep exits.",
            "nosleep - Settings save failed", MB_OK | MB_ICONWARNING);
    }
    return success;
}

bool tray_save_settings_cli(int session_finished_action,
                            int auto_start,
                            int notification_mode,
                            int auto_check_interval,
                            int check_updates_startup,
                            int add_to_path) {
    // Validate enum ranges
    if (session_finished_action >= 0 &&
        session_finished_action != SESSION_FINISHED_NONE &&
        session_finished_action != SESSION_FINISHED_SHUTDOWN &&
        session_finished_action != SESSION_FINISHED_SLEEP &&
        session_finished_action != SESSION_FINISHED_SHUTDOWN_GRACEFUL) {
        return false;
    }
    if (notification_mode >= 0 &&
        notification_mode != NOTIFY_ALL &&
        notification_mode != NOTIFY_CRITICAL_ONLY &&
        notification_mode != NOTIFY_NONE) {
        return false;
    }
    if (auto_check_interval >= 0 &&
        auto_check_interval != 0 &&
        auto_check_interval != 1 &&
        auto_check_interval != 2) {
        return false;
    }
    if (auto_start < -1 || auto_start > 1) return false;
    if (check_updates_startup < -1 || check_updates_startup > 1) return false;
    if (add_to_path < -1 || add_to_path > 1) return false;

    HKEY hKey = NULL;
    bool success = true;
    if (session_finished_action >= 0 || notification_mode >= 0 ||
        auto_check_interval >= 0 || check_updates_startup >= 0 || add_to_path >= 0) {
        LONG result = RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY,
            0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
        if (result != ERROR_SUCCESS) {
            hKey = NULL;
            success = false;
        }
    }
    if (hKey && session_finished_action >= 0) {
        DWORD val = (DWORD)session_finished_action;
        if (RegSetValueEx(hKey, "session_finished_action", 0, REG_DWORD,
                          (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
            success = false;
        }
    }
    if (auto_start >= 0 && !set_startup_registry(auto_start != 0)) {
        success = false;
    }
    if (hKey && notification_mode >= 0) {
        DWORD val = (DWORD)notification_mode;
        if (RegSetValueEx(hKey, "notification_mode", 0, REG_DWORD,
                          (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
            success = false;
        }
    }
    if (hKey && auto_check_interval >= 0) {
        DWORD val = (DWORD)auto_check_interval;
        if (RegSetValueEx(hKey, "auto_check_interval", 0, REG_DWORD,
                          (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
            success = false;
        }
    }
    if (hKey && check_updates_startup >= 0) {
        DWORD val = (DWORD)(check_updates_startup != 0 ? 1 : 0);
        if (RegSetValueEx(hKey, "check_updates_on_startup", 0, REG_DWORD,
                          (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
            success = false;
        }
    }
    if (add_to_path >= 0) {
        DWORD val = (DWORD)(add_to_path != 0 ? 1 : 0);
        if (hKey && RegSetValueEx(hKey, "add_to_path", 0, REG_DWORD,
                          (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
            success = false;
        }
        if (!apply_path_preference(add_to_path != 0)) {
            success = false;
        }
    }

    if (hKey) RegCloseKey(hKey);
    return success;
}

static void save_last_update_check_time(void) {
    HKEY hKey;
    LONG result = RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY,
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (result != ERROR_SUCCESS) return;

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    ULONGLONG time_val = uli.QuadPart;
    RegSetValueEx(hKey, "last_update_check", 0, REG_QWORD, (LPBYTE)&time_val, sizeof(time_val));

    RegCloseKey(hKey);
}

static bool should_check_for_updates(void) {
    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY,
        0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) return true;

    DWORD interval = 1;
    DWORD size = sizeof(DWORD);
    RegQueryValueEx(hKey, "auto_check_interval", NULL, NULL, (LPBYTE)&interval, &size);

    if (interval == 0) {
        RegCloseKey(hKey);
        return false;
    }

    ULONGLONG last_check = 0;
    DWORD qsize = sizeof(ULONGLONG);
    result = RegQueryValueEx(hKey, "last_update_check", NULL, NULL, (LPBYTE)&last_check, &qsize);
    RegCloseKey(hKey);

    if (result != ERROR_SUCCESS) return true;

    FILETIME ft_now;
    GetSystemTimeAsFileTime(&ft_now);
    ULARGE_INTEGER uli_now;
    uli_now.LowPart = ft_now.dwLowDateTime;
    uli_now.HighPart = ft_now.dwHighDateTime;
    ULONGLONG now = uli_now.QuadPart;

    // A clock rollback must not wrap the unsigned elapsed time.
    if (last_check > now) return false;

    ULONGLONG interval_100ns;
    if (interval == 1) {
        interval_100ns = (ULONGLONG)24 * 60 * 60 * 10000000LL;
    } else {
        interval_100ns = (ULONGLONG)7 * 24 * 60 * 60 * 10000000LL;
    }

    return (now - last_check) >= interval_100ns;
}

void tray_set_startup_enabled(NoSleepTray* tray, bool enable) {
    if (!tray) return;
    if (set_startup_registry(enable)) {
        tray->start_on_startup = enable;
    }
}

bool tray_set_add_to_path(NoSleepTray* tray, bool enable) {
    if (!tray) return false;
    // Keep the desired preference even if the PATH update fails so startup
    // can retry applying it.
    tray->add_to_path = enable;
    return apply_path_preference(enable);
}

void tray_show_notification(NoSleepTray* tray, NotifyEventId event_type,
                            const char* title, const char* message, bool critical) {
    if (!tray || !tray->hwnd) return;
    (void)critical; // Retained for existing callers; event groups define visibility.

    // The active event group is authoritative for direct and event-originated notifications.
    if (!notify_groups_should_show(&tray->notify_groups, event_type)) return;
    
    DEBUG_LOG("tray_show_notification: title='%s', message='%s'", title, message);
    
    char full_title[256];
    sprintf(full_title, "nosleep - %s", title);

    AcquireSRWLockExclusive(&tray->tray_icon_lock);
    
    // Use balloon notification
    tray->nid.uFlags |= NIF_INFO;
    strcpy(tray->nid.szInfoTitle, full_title);
    strcpy(tray->nid.szInfo, message);
    tray->nid.dwInfoFlags = NIIF_INFO;
    tray->nid.uTimeout = 3000; // 3 seconds
    
    Shell_NotifyIcon(NIM_MODIFY, &tray->nid);
    
    // Reset flags
    tray->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    ReleaseSRWLockExclusive(&tray->tray_icon_lock);
}

// Older SDK targets omit this message even though newer systems send it.
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// Resolve newer DPI APIs at runtime so older Windows versions keep working.
static UINT custom_dialog_dpi(HWND hwnd) {
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    GetDpiForWindowFn get_dpi = (GetDpiForWindowFn)GetProcAddress(
        GetModuleHandle("user32.dll"), "GetDpiForWindow");
    UINT dpi = get_dpi ? get_dpi(hwnd) : 0;
    if (!dpi) {
        HDC dc = GetDC(hwnd);
        dpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(hwnd, dc);
    }
    return dpi ? dpi : 96;
}

static void layout_custom_dialog(HWND hwnd, UINT dpi, HFONT* font) {
    const int ids[] = {3, 4, 1, 2};
    const int geometry[][4] = {
        {20, 10, 220, 20}, {20, 35, 220, 25},
        {20, 75, 100, 30}, {140, 75, 100, 30}
    };
    for (int i = 0; i < 4; ++i) {
        MoveWindow(GetDlgItem(hwnd, ids[i]),
            MulDiv(geometry[i][0], dpi, 96), MulDiv(geometry[i][1], dpi, 96),
            MulDiv(geometry[i][2], dpi, 96), MulDiv(geometry[i][3], dpi, 96), TRUE);
    }
    HFONT replacement = create_dialog_font(MulDiv(14, dpi, 96));
    if (replacement) {
        EnumChildWindows(hwnd, set_child_font_proc, (LPARAM)replacement);
        if (*font) DeleteObject(*font);
        *font = replacement;
    }
}

static void size_custom_dialog(HWND hwnd, UINT dpi) {
    typedef BOOL (WINAPI *AdjustWindowRectExForDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    AdjustWindowRectExForDpiFn adjust = (AdjustWindowRectExForDpiFn)GetProcAddress(
        GetModuleHandle("user32.dll"), "AdjustWindowRectExForDpi");
    RECT rect = {0, 0, MulDiv(260, dpi, 96), MulDiv(125, dpi, 96)};
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    DWORD ex_style = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (!adjust || !adjust(&rect, style, FALSE, ex_style, dpi)) {
        AdjustWindowRectEx(&rect, style, FALSE, ex_style);
    }
    SetWindowPos(hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Simple input dialog window procedure
static LRESULT CALLBACK input_dialog_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit = NULL;
    static int* pResult = NULL;
    static HFONT hInputFont = NULL;
    
    switch (msg) {
        case WM_CREATE:
            {
                CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
                pResult = (int*)cs->lpCreateParams;
                char initial_duration[16];
                snprintf(initial_duration, sizeof(initial_duration), "%d", pResult ? *pResult : 30);
                if (pResult) {
                    *pResult = -1;
                }
                
                // Create edit control
                hEdit = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", initial_duration,
                    WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_TABSTOP,
                    20, 30, 150, 25,
                    hwnd, (HMENU)4, GetModuleHandle(NULL), NULL);
                
                if (hEdit) {
                    // Limit input to 4 characters
                    SendMessage(hEdit, EM_SETLIMITTEXT, 4, 0);
                    SetFocus(hEdit);
                    SendMessage(hEdit, EM_SETSEL, 0, -1);
                }
            }
            return 0;
            
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case 1: // OK button
                    if (hEdit) {
                        char buffer[256];
                        GetWindowText(hEdit, buffer, sizeof(buffer));
                        char* endptr;
                        long minutes = strtol(buffer, &endptr, 10);
                        if (endptr != buffer && *endptr == '\0' && minutes >= 1 && minutes <= 1440) {
                            if (pResult) {
                                *pResult = (int)minutes;
                            }
                            DestroyWindow(hwnd);
                        } else {
                            MessageBox(hwnd, "Please enter a valid number between 1 and 1440 minutes.",
                                      "Invalid Input", MB_OK | MB_ICONWARNING);
                            SetFocus(hEdit);
                            SendMessage(hEdit, EM_SETSEL, 0, -1);
                        }
                    }
                    return TRUE;
                    
                case 2: // Cancel button
                    DestroyWindow(hwnd);
                    return TRUE;
            }
            break;
            
        case WM_DPICHANGED:
            {
                RECT* suggested = (RECT*)lParam;
                SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                    suggested->right - suggested->left, suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                size_custom_dialog(hwnd, HIWORD(wParam));
                layout_custom_dialog(hwnd, HIWORD(wParam), &hInputFont);
            }
            return 0;

        case WM_APP:
            layout_custom_dialog(hwnd, custom_dialog_dpi(hwnd), &hInputFont);
            return 0;

        case WM_DESTROY:
            if (hInputFont) {
                DeleteObject(hInputFont);
                hInputFont = NULL;
            }
            break;
            
        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static int tray_show_custom_dialog(NoSleepTray* tray) {
    // Create a simple input dialog using CreateWindow
    HINSTANCE hInstance = GetModuleHandle(NULL);
    static int last_custom_duration = 30;
    static bool custom_duration_loaded = false;
    if (!custom_duration_loaded) {
        last_custom_duration = settings_load_custom_duration();
        custom_duration_loaded = true;
    }
    int result = last_custom_duration;
    
    // Register dialog window class
    WNDCLASS wc = {0};
    wc.lpfnWndProc = input_dialog_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "NoSleepInputDialog";
    
    if (!RegisterClass(&wc)) {
        return -1;
    }
    
    typedef HANDLE (WINAPI *SetThreadDpiAwarenessContextFn)(HANDLE);
    SetThreadDpiAwarenessContextFn set_dpi_context =
        (SetThreadDpiAwarenessContextFn)GetProcAddress(
            GetModuleHandle("user32.dll"), "SetThreadDpiAwarenessContext");
    HANDLE previous_context = NULL;
    if (set_dpi_context) {
        previous_context = set_dpi_context((HANDLE)(INT_PTR)-4);
        if (!previous_context) previous_context = set_dpi_context((HANDLE)(INT_PTR)-3);
    }

    // Use the tray interaction's monitor, including its taskbar work-area inset.
    POINT cursor;
    HMONITOR monitor = GetCursorPos(&cursor)
        ? MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST)
        : MonitorFromWindow(tray->hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {0};
    monitor_info.cbSize = sizeof(monitor_info);
    RECT work_area;
    if (GetMonitorInfo(monitor, &monitor_info)) {
        work_area = monitor_info.rcWork;
    } else if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &work_area, 0)) {
        work_area = (RECT){0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    // Create dialog window
    HWND hwndDlg = CreateWindowEx(
        0,
        "NoSleepInputDialog",
        "Custom Duration - nosleep",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        work_area.left, work_area.top,
        260, 150,
        tray->hwnd,
        NULL,
        hInstance,
        (LPVOID)&result
    );
    
    if (!hwndDlg) {
        if (previous_context) set_dpi_context(previous_context);
        UnregisterClass("NoSleepInputDialog", hInstance);
        return -1;
    }
    
    // Create OK button
    CreateWindowEx(0, "BUTTON", "OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        20, 70, 80, 30,
        hwndDlg, (HMENU)1, hInstance, NULL);
    
    // Create Cancel button
    CreateWindowEx(0, "BUTTON", "Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        120, 70, 80, 30,
        hwndDlg, (HMENU)2, hInstance, NULL);
    
    // Create static text
    CreateWindowEx(0, "STATIC", "Enter duration (minutes, 1-1440):",
        WS_CHILD | WS_VISIBLE,
        20, 10, 180, 20,
        hwndDlg, (HMENU)3, hInstance, NULL);

    UINT dpi = custom_dialog_dpi(hwndDlg);
    size_custom_dialog(hwndDlg, dpi);
    SendMessage(hwndDlg, WM_APP, 0, 0);
    RECT bounds;
    GetWindowRect(hwndDlg, &bounds);
    int width = bounds.right - bounds.left;
    int height = bounds.bottom - bounds.top;
    int x = work_area.left + (work_area.right - work_area.left - width) / 2;
    int y = work_area.top + (work_area.bottom - work_area.top - height) / 2;
    if (x < work_area.left) x = work_area.left;
    if (y < work_area.top) y = work_area.top;
    SetWindowPos(hwndDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Show dialog
    ShowWindow(hwndDlg, SW_SHOW);
    
    // Normal dialog closure ends this loop without consuming application quit.
    MSG msg;
    int message_result = 1;
    while (IsWindow(hwndDlg) && (message_result = GetMessage(&msg, NULL, 0, 0)) > 0) {
        if (!IsDialogMessage(hwndDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    if (message_result <= 0) {
        result = -1;
        if (IsWindow(hwndDlg)) DestroyWindow(hwndDlg);
        if (message_result == 0) PostQuitMessage((int)msg.wParam);
    }

    // Cleanup
    if (previous_context) set_dpi_context(previous_context);
    UnregisterClass("NoSleepInputDialog", hInstance);
    if (result > 0) {
        last_custom_duration = result;
        settings_save_custom_duration(result);
    }
    
    return result;
}

// Settings dialog control IDs
#define IDC_PREVENT_DISPLAY      2001
#define IDC_AWAY_MODE            2002
#define IDC_AUTO_START           2003
#define IDC_VERBOSE_LOGGING      2004
#define IDC_CHECK_UPDATES_STARTUP 2005
#define IDC_AUTO_CHECK_INTERVAL  2006
#define IDC_AUTO_CHECK_STATUS    2009
#define IDC_AUTO_CHECK_RETRY     2010
#define IDC_SETTINGS_OK          2007
#define IDC_SETTINGS_CANCEL      2008
#define IDC_ADD_TO_PATH          2012
#define IDC_SETTINGS_TAB         2013
#define IDC_NOTIFY_GROUP_LIST    2014

// Notification group edit popup control IDs
#define IDC_NOTIFY_GROUP_NAME       2100
#define IDC_NOTIFY_EVENT_CHECK_START 2101
#define IDC_NOTIFY_GROUP_EDIT_OK    2200
#define IDC_NOTIFY_GROUP_EDIT_CANCEL 2201
#define IDC_NOTIFY_ADD_GROUP        2202
#define IDC_NOTIFY_DEL_GROUP        2203
#define IDC_NOTIFY_CONFIGURE_GROUP  2204
#define IDC_NOTIFY_SET_ACTIVE       2205
#define IDC_NOTIFY_EVENT_SELECT_ALL 2206
#define IDC_NOTIFY_EVENT_CLEAR_ALL  2207
#define IDC_NOTIFY_RESTORE_DEFAULT  2208

// Tab indices
#define TAB_GENERAL       0
#define TAB_NOTIFICATIONS 1

// Forward declarations
static LRESULT CALLBACK notify_group_edit_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void create_general_tab(HWND hwnd_tab, NoSleepTray* tray);
static void create_notifications_tab(HWND hwnd_tab, NoSleepTray* tray);
static void refresh_notification_group_list(HWND hwnd_tab, NoSleepTray* tray);
static void update_notification_group_actions(HWND hwnd_tab, NoSleepTray* tray);

// Keep the original 96-DPI geometry so repeated monitor changes do not
// accumulate rounding errors, including controls nested inside tab pages.
typedef struct DialogControlLayout {
    HWND hwnd;
    RECT bounds;
    struct DialogControlLayout* next;
} DialogControlLayout;

typedef struct {
    DialogControlLayout* controls;
    BOOL failed;
    int width;
    int height;
} DialogLayout;

static BOOL CALLBACK capture_dialog_control(HWND child, LPARAM param) {
    DialogLayout* layout = (DialogLayout*)param;
    DialogControlLayout* control = calloc(1, sizeof(*control));
    if (!control) {
        layout->failed = TRUE;
        return FALSE;
    }
    control->hwnd = child;
    GetWindowRect(child, &control->bounds);
    // Combo boxes need their full drop-down height when repositioned.
    char class_name[32];
    if (GetClassName(child, class_name, sizeof(class_name)) &&
        lstrcmpi(class_name, "COMBOBOX") == 0) {
        RECT dropped;
        if (SendMessage(child, CB_GETDROPPEDCONTROLRECT, 0, (LPARAM)&dropped)) {
            control->bounds.bottom = control->bounds.top + dropped.bottom - dropped.top;
        }
    }
    MapWindowPoints(NULL, GetParent(child), (POINT*)&control->bounds, 2);
    control->next = layout->controls;
    layout->controls = control;
    return TRUE;
}

static void free_dialog_layout(HWND hwnd) {
    DialogLayout* layout = (DialogLayout*)RemoveProp(hwnd, "NoSleepDialogLayout");
    if (!layout) return;
    while (layout->controls) {
        DialogControlLayout* next = layout->controls->next;
        free(layout->controls);
        layout->controls = next;
    }
    free(layout);
}

static void scale_dialog_layout(HWND hwnd, UINT dpi, const RECT* suggested) {
    DialogLayout* layout = (DialogLayout*)GetProp(hwnd, "NoSleepDialogLayout");
    if (!layout || !dpi) return;
    for (DialogControlLayout* control = layout->controls; control; control = control->next) {
        RECT* r = &control->bounds;
        MoveWindow(control->hwnd, MulDiv(r->left, dpi, 96), MulDiv(r->top, dpi, 96),
            MulDiv(r->right - r->left, dpi, 96), MulDiv(r->bottom - r->top, dpi, 96), TRUE);
    }
    HFONT replacement = create_dialog_font(MulDiv(14, dpi, 96));
    if (replacement) {
        EnumChildWindows(hwnd, set_child_font_proc, (LPARAM)replacement);
        HFONT previous = (HFONT)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)replacement);
        if (previous) DeleteObject(previous);
    }
    typedef BOOL (WINAPI *AdjustForDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    AdjustForDpiFn adjust = (AdjustForDpiFn)GetProcAddress(
        GetModuleHandle("user32.dll"), "AdjustWindowRectExForDpi");
    RECT r = {0, 0, MulDiv(layout->width, dpi, 96), MulDiv(layout->height, dpi, 96)};
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    DWORD ex_style = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (!adjust || !adjust(&r, style, FALSE, ex_style, dpi)) {
        AdjustWindowRectEx(&r, style, FALSE, ex_style);
    }
    SetWindowPos(hwnd, NULL, suggested ? suggested->left : 0, suggested ? suggested->top : 0,
        r.right - r.left, r.bottom - r.top,
        SWP_NOZORDER | SWP_NOACTIVATE | (suggested ? 0 : SWP_NOMOVE));
    InvalidateRect(hwnd, NULL, TRUE);
}

static BOOL initialize_dialog_layout(HWND hwnd, int width, int height) {
    DialogLayout* layout = calloc(1, sizeof(*layout));
    if (!layout) return FALSE;
    layout->width = width;
    layout->height = height;
    if (!SetProp(hwnd, "NoSleepDialogLayout", (HANDLE)layout)) {
        free(layout);
        return FALSE;
    }
    EnumChildWindows(hwnd, capture_dialog_control, (LPARAM)layout);
    if (layout->failed) {
        free_dialog_layout(hwnd);
        return FALSE;
    }
    scale_dialog_layout(hwnd, custom_dialog_dpi(hwnd), NULL);
    RECT bounds;
    GetWindowRect(hwnd, &bounds);
    MONITORINFO monitor = {0};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        int x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left -
            (bounds.right - bounds.left)) / 2;
        int y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top -
            (bounds.bottom - bounds.top)) / 2;
        SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return TRUE;
}

// Modern systems receive per-monitor notifications; older systems use their
// existing awareness and the device-context DPI fallback.
typedef HANDLE (WINAPI *DialogDpiContextFn)(HANDLE);
static HANDLE enter_dialog_dpi_context(DialogDpiContextFn* setter) {
    *setter = (DialogDpiContextFn)GetProcAddress(
        GetModuleHandle("user32.dll"), "SetThreadDpiAwarenessContext");
    if (!*setter) return NULL;
    HANDLE previous = (*setter)((HANDLE)(INT_PTR)-4);
    if (!previous) previous = (*setter)((HANDLE)(INT_PTR)-3);
    return previous;
}

// Global for passing group edit info
static struct {
    NotifyGroupManager* mgr;
    int group_index;
    HWND hwnd_parent;
} g_notify_edit_ctx = {NULL, -1, NULL};

static LRESULT CALLBACK notify_tab_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                                  LPARAM lParam, UINT_PTR subclass_id,
                                                  DWORD_PTR ref_data) {
    (void)subclass_id;
    (void)ref_data;

    if (msg == WM_COMMAND) {
        return SendMessage(GetParent(hwnd), msg, wParam, lParam);
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Report the applied preference, independently of the combo's unsaved selection.
static void refresh_auto_check_status(HWND general_tab, NoSleepTray* tray) {
    const char* status;
    bool failed = tray->auto_check_interval != 0 && !tray->update_timer_id;
    if (tray->auto_check_interval == 0) {
        status = "Applied schedule: disabled.";
    } else if (failed) {
        status = tray->auto_check_interval == 1
            ? "Applied: Daily. Not scheduled; retry below."
            : "Applied: Weekly. Not scheduled; retry below.";
    } else {
        status = tray->auto_check_interval == 1
            ? "Applied schedule: Daily, active."
            : "Applied schedule: Weekly, active.";
    }
    SetDlgItemText(general_tab, IDC_AUTO_CHECK_STATUS, status);
    EnableWindow(GetDlgItem(general_tab, IDC_AUTO_CHECK_RETRY), failed);
}

static LRESULT CALLBACK settings_dialog_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static NoSleepTray* settings_tray = NULL;
    static HWND hTab = NULL;
    static HWND hGeneralTab = NULL;
    static HWND hNotifyTab = NULL;
    static HWND hIntervalCombo = NULL;

    switch (msg) {
        case WM_CREATE:
        {
            CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
            settings_tray = (NoSleepTray*)cs->lpCreateParams;
            if (!settings_tray) return -1;

            HINSTANCE hInst = GetModuleHandle(NULL);

            // Register common controls for tab control
            INITCOMMONCONTROLSEX icex;
            icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
            icex.dwICC = ICC_TAB_CLASSES;
            InitCommonControlsEx(&icex);

            // Create tab control
            hTab = CreateWindowEx(0, WC_TABCONTROL, NULL,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FIXEDWIDTH,
                10, 10, 460, 410,
                hwnd, (HMENU)IDC_SETTINGS_TAB, hInst, NULL);

            // Add tabs
            TCITEM tie = {0};
            tie.mask = TCIF_TEXT;
            
            char tab1_text[] = "General";
            tie.pszText = tab1_text;
            TabCtrl_InsertItem(hTab, TAB_GENERAL, &tie);
            
            char tab2_text[] = "Notifications";
            tie.pszText = tab2_text;
            TabCtrl_InsertItem(hTab, TAB_NOTIFICATIONS, &tie);

            // Create tab page windows (initially hidden, shown when tab selected)
            // General tab
            hGeneralTab = CreateWindowEx(WS_EX_CONTROLPARENT, "STATIC", NULL,
                WS_CHILD | WS_VISIBLE,
                15, 35, 450, 380,
                hwnd, NULL, hInst, NULL);
            if (!SetWindowSubclass(hGeneralTab, notify_tab_subclass_proc, 1, 0)) {
                return -1;
            }
            create_general_tab(hGeneralTab, settings_tray);
            
            // Notifications tab (initially hidden)
            hNotifyTab = CreateWindowEx(WS_EX_CONTROLPARENT, "STATIC", NULL,
                WS_CHILD,  // Not visible initially
                15, 35, 450, 380,
                hwnd, NULL, hInst, NULL);
            // Notification controls send WM_COMMAND to their immediate parent,
            // so forward those messages to the dialog's command handlers.
            if (!SetWindowSubclass(hNotifyTab, notify_tab_subclass_proc, 1, 0)) {
                return -1;
            }
            create_notifications_tab(hNotifyTab, settings_tray);

            // Show the first tab by default
            ShowWindow(hGeneralTab, SW_SHOW);
            ShowWindow(hNotifyTab, SW_HIDE);

            // Find the interval combo (created inside the general tab's static parent)
            hIntervalCombo = GetDlgItem(hGeneralTab, IDC_AUTO_CHECK_INTERVAL);

            // OK and Cancel buttons at the bottom (outside tab control)
            CreateWindowEx(0, "BUTTON", "OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                150, 430, 80, 28, hwnd, (HMENU)IDC_SETTINGS_OK, hInst, NULL);

            CreateWindowEx(0, "BUTTON", "Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                250, 430, 80, 28, hwnd, (HMENU)IDC_SETTINGS_CANCEL, hInst, NULL);

            if (!initialize_dialog_layout(hwnd, 480, 470)) return -1;

            return 0;
        }

        case WM_NOTIFY:
        {
            if (((LPNMHDR)lParam)->idFrom == IDC_SETTINGS_TAB &&
                ((LPNMHDR)lParam)->code == TCN_SELCHANGE) {
                int sel = TabCtrl_GetCurSel(hTab);
                ShowWindow(hGeneralTab, sel == TAB_GENERAL ? SW_SHOW : SW_HIDE);
                ShowWindow(hNotifyTab, sel == TAB_NOTIFICATIONS ? SW_SHOW : SW_HIDE);
                if (sel == TAB_NOTIFICATIONS) {
                    refresh_notification_group_list(hNotifyTab, settings_tray);
                }
            }
            break;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_AUTO_CHECK_RETRY:
                    if (settings_tray && HIWORD(wParam) == BN_CLICKED) {
                        // Retry the applied schedule without saving pending preferences.
                        tray_apply_auto_check_interval(settings_tray, settings_tray->auto_check_interval);
                        refresh_auto_check_status(hGeneralTab, settings_tray);
                    }
                    break;

                case IDOK: // Enter from IsDialogMessage
                case IDC_SETTINGS_OK:
                {
                    if (!settings_tray) break;
                    // Stage general preferences without changing live state or timers.
                    NoSleepTray proposed = *settings_tray;
                    proposed.prevent_display = (SendDlgItemMessage(hGeneralTab, IDC_PREVENT_DISPLAY, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    proposed.away_mode = (SendDlgItemMessage(hGeneralTab, IDC_AWAY_MODE, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    proposed.verbose = (SendDlgItemMessage(hGeneralTab, IDC_VERBOSE_LOGGING, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    proposed.check_updates_on_startup = (SendDlgItemMessage(hGeneralTab, IDC_CHECK_UPDATES_STARTUP, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    if (hIntervalCombo) {
                        int sel = (int)SendMessage(hIntervalCombo, CB_GETCURSEL, 0, 0);
                        if (sel != CB_ERR) proposed.auto_check_interval = sel;
                    }
                    proposed.add_to_path = (SendDlgItemMessage(hGeneralTab, IDC_ADD_TO_PATH, BM_GETCHECK, 0, 0) == BST_CHECKED);

                    bool auto_start = (SendDlgItemMessage(hGeneralTab, IDC_AUTO_START, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    bool startup_changed = auto_start != settings_tray->start_on_startup;
                    if (startup_changed) {
                        tray_set_startup_enabled(settings_tray, auto_start);
                        if (settings_tray->start_on_startup != auto_start) {
                            MessageBox(hwnd,
                                "The startup setting could not be changed. Please try again.",
                                "nosleep - Startup update failed", MB_OK | MB_ICONWARNING);
                            break;
                        }
                    }

                    if (!tray_save_settings(&proposed)) {
                        MessageBox(hwnd,
                            startup_changed
                                ? "Some settings could not be saved. General preferences were not applied to this session, but some saved values may have changed. The startup setting was changed and Cancel will not undo it."
                                : "Some settings could not be saved. General preferences were not applied to this session, but some saved values may have changed. Please try again.",
                            "nosleep - Settings save failed", MB_OK | MB_ICONWARNING);
                        break;
                    }

                    settings_tray->prevent_display = proposed.prevent_display;
                    settings_tray->away_mode = proposed.away_mode;
                    settings_tray->verbose = proposed.verbose;
                    settings_tray->check_updates_on_startup = proposed.check_updates_on_startup;
                    tray_apply_auto_check_interval(settings_tray, proposed.auto_check_interval);
                    if (proposed.add_to_path != settings_tray->add_to_path &&
                        !tray_set_add_to_path(settings_tray, proposed.add_to_path)) {
                        MessageBox(hwnd,
                            "The PATH change failed. NoSleep will retry applying this setting the next time it starts.",
                            "nosleep - PATH update failed", MB_OK | MB_ICONWARNING);
                    }
                    if (!settings_tray || !IsWindow(hwnd) || !IsWindow(hGeneralTab)) break;
                    refresh_auto_check_status(hGeneralTab, settings_tray);
                    if (settings_tray->auto_check_interval != 0 && !settings_tray->update_timer_id) {
                        MessageBox(hwnd,
                            "Preferences were saved, but automatic update checks could not be scheduled. Use Retry applied schedule to try again. Cancel closes Settings without undoing the saved preferences.",
                            "nosleep - Updates not scheduled", MB_OK | MB_ICONWARNING);
                        break;
                    }
                    DestroyWindow(hwnd);
                    break;
                }

                case IDCANCEL: // Escape from IsDialogMessage
                case IDC_SETTINGS_CANCEL:
                    DestroyWindow(hwnd);
                    break;

                case IDC_NOTIFY_GROUP_LIST:
                    if (HIWORD(wParam) == LBN_SELCHANGE) {
                        update_notification_group_actions(hNotifyTab, settings_tray);
                    }
                    break;

                case IDC_NOTIFY_ADD_GROUP:
                {
                    int previous_count = settings_tray->notify_groups.count;
                    show_notify_group_edit_dialog(hwnd, &settings_tray->notify_groups, -1);
                    // The nested editor can dispatch Settings Cancel/Close.
                    if (!IsWindow(hwnd) || !settings_tray || !IsWindow(hNotifyTab)) break;
                    refresh_notification_group_list(hNotifyTab, settings_tray);
                    // A saved new group is appended; cancel and failed saves leave count unchanged.
                    if (settings_tray->notify_groups.count > previous_count) {
                        HWND hList = GetDlgItem(hNotifyTab, IDC_NOTIFY_GROUP_LIST);
                        if (hList) {
                            int rows = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
                            for (int row = 0; row < rows; row++) {
                                int group_idx = (int)SendMessage(hList, LB_GETITEMDATA, (WPARAM)row, 0);
                                if (group_idx == previous_count) {
                                    SendMessage(hList, LB_SETCURSEL, (WPARAM)row, 0);
                                    break;
                                }
                            }
                        }
                        update_notification_group_actions(hNotifyTab, settings_tray);
                    }
                    break;
                }

                case IDC_NOTIFY_CONFIGURE_GROUP:
                {
                    HWND hList = GetDlgItem(hNotifyTab, IDC_NOTIFY_GROUP_LIST);
                    if (!hList) break;
                    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
                    if (sel == LB_ERR) break;
                    
                    int group_idx = (int)SendMessage(hList, LB_GETITEMDATA, (WPARAM)sel, 0);
                    if (group_idx < 0 || group_idx >= settings_tray->notify_groups.count) break;
                    
                    show_notify_group_edit_dialog(hwnd, &settings_tray->notify_groups, group_idx);
                    refresh_notification_group_list(hNotifyTab, settings_tray);
                    break;
                }

                case IDC_NOTIFY_RESTORE_DEFAULT:
                {
                    HWND hList = GetDlgItem(hNotifyTab, IDC_NOTIFY_GROUP_LIST);
                    if (!hList) break;
                    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
                    if (sel == LB_ERR) break;
                    int group_idx = (int)SendMessage(hList, LB_GETITEMDATA, (WPARAM)sel, 0);
                    if (group_idx < 0 || group_idx >= settings_tray->notify_groups.count ||
                        !settings_tray->notify_groups.groups[group_idx].is_default) break;

                    // Legacy groups retain only a default flag, not their original preset.
                    HMENU presets = CreatePopupMenu();
                    if (!presets) break;
                    AppendMenu(presets, MF_STRING, 1, "All notifications");
                    AppendMenu(presets, MF_STRING, 2, "Critical only");
                    AppendMenu(presets, MF_STRING, 3, "None");
                    RECT button;
                    GetWindowRect(GetDlgItem(hNotifyTab, IDC_NOTIFY_RESTORE_DEFAULT), &button);
                    int choice = TrackPopupMenu(presets, TPM_RETURNCMD | TPM_NONOTIFY,
                        button.left, button.bottom, 0, hwnd, NULL);
                    DestroyMenu(presets);
                    // Menu tracking dispatches messages, including Settings Close.
                    if (!IsWindow(hwnd) || !settings_tray || !IsWindow(hNotifyTab) ||
                        choice < 1 || choice > 3) break;

                    NotifyGroupManager updated = settings_tray->notify_groups;
                    if (!notify_groups_restore_default(&updated, group_idx, choice - 1)) {
                        MessageBox(hwnd,
                            "A group already uses this preset name. Rename that group before restoring this default.",
                            "nosleep - Cannot restore default", MB_OK | MB_ICONWARNING);
                        break;
                    }
                    if (notify_groups_save(&updated)) {
                        settings_tray->notify_groups = updated;
                    } else {
                        MessageBox(hwnd,
                            "Could not save notification groups. Please check registry access and try again.",
                            "nosleep - Notification group save failed", MB_OK | MB_ICONWARNING);
                    }
                    refresh_notification_group_list(hNotifyTab, settings_tray);
                    break;
                }

                case IDC_NOTIFY_SET_ACTIVE:
                {
                    HWND hList = GetDlgItem(hNotifyTab, IDC_NOTIFY_GROUP_LIST);
                    if (!hList) break;
                    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
                    if (sel == LB_ERR) break;
                    
                    int group_idx = (int)SendMessage(hList, LB_GETITEMDATA, (WPARAM)sel, 0);
                    if (group_idx < 0 || group_idx >= settings_tray->notify_groups.count) break;
                    
                    NotifyGroupManager updated = settings_tray->notify_groups;
                    if (notify_groups_set_active(&updated, group_idx)) {
                        if (notify_groups_save(&updated)) {
                            settings_tray->notify_groups = updated;
                        } else {
                            MessageBox(hwnd,
                                "Could not save notification groups. Please check registry access and try again.",
                                "nosleep - Notification group save failed", MB_OK | MB_ICONWARNING);
                        }
                        refresh_notification_group_list(hNotifyTab, settings_tray);
                    }
                    break;
                }

                case IDC_NOTIFY_DEL_GROUP:
                {
                    HWND hList = GetDlgItem(hNotifyTab, IDC_NOTIFY_GROUP_LIST);
                    if (!hList) break;
                    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
                    if (sel == LB_ERR) break;
                    
                    int group_idx = (int)SendMessage(hList, LB_GETITEMDATA, (WPARAM)sel, 0);
                    if (group_idx < 0 || group_idx >= settings_tray->notify_groups.count) break;
                    
                    // Cannot delete default groups
                    if (settings_tray->notify_groups.groups[group_idx].is_default) {
                        MessageBox(hwnd, "Default notification groups cannot be deleted.",
                            "Cannot Delete", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
                        break;
                    }
                    
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Delete notification group \"%s\"?",
                             settings_tray->notify_groups.groups[group_idx].name);
                    int confirm = MessageBox(hwnd, msg, "Confirm Delete",
                        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
                    if (confirm == IDYES) {
                        NotifyGroupManager updated = settings_tray->notify_groups;
                        if (notify_groups_remove(&updated, group_idx)) {
                            if (notify_groups_save(&updated)) {
                                settings_tray->notify_groups = updated;
                            } else {
                                MessageBox(hwnd,
                                    "Could not save notification groups. Please check registry access and try again.",
                                    "nosleep - Notification group save failed", MB_OK | MB_ICONWARNING);
                            }
                            refresh_notification_group_list(hNotifyTab, settings_tray);
                        }
                    }
                    break;
                }
            }
            break;

        case WM_DPICHANGED:
            scale_dialog_layout(hwnd, HIWORD(wParam), (const RECT*)lParam);
            refresh_notification_group_list(hNotifyTab, settings_tray);
            return 0;

        case WM_NCDESTROY:
            free_dialog_layout(hwnd);
            break;

        case WM_DESTROY:
            {
                HFONT hFont = (HFONT)GetWindowLongPtr(hwnd, GWLP_USERDATA);
                if (hFont) DeleteObject(hFont);
            }
            settings_tray = NULL;
            hTab = NULL;
            hGeneralTab = NULL;
            hNotifyTab = NULL;
            hIntervalCombo = NULL;
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Create controls for the "General" tab
static void create_general_tab(HWND hwnd_parent, NoSleepTray* tray) {
    HINSTANCE hInst = GetModuleHandle(NULL);
    int y = 10;

    CreateWindowEx(0, "BUTTON", "Prevent display sleep",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        20, y, 240, 25, hwnd_parent, (HMENU)IDC_PREVENT_DISPLAY, hInst, NULL);
    if (tray->prevent_display) {
        SendDlgItemMessage(hwnd_parent, IDC_PREVENT_DISPLAY, BM_SETCHECK, BST_CHECKED, 0);
    }
    y += 25;
    CreateWindowEx(0, "STATIC", "Keeps the display from idling off during an active session.",
        WS_CHILD | WS_VISIBLE,
        40, y, 390, 20, hwnd_parent, NULL, hInst, NULL);
    y += 27;

    CreateWindowEx(0, "BUTTON", "Enable away mode",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        20, y, 240, 25, hwnd_parent, (HMENU)IDC_AWAY_MODE, hInst, NULL);
    if (tray->away_mode) {
        SendDlgItemMessage(hwnd_parent, IDC_AWAY_MODE, BM_SETCHECK, BST_CHECKED, 0);
    }
    y += 25;
    CreateWindowEx(0, "STATIC", "On supported PCs, Windows can appear asleep while NoSleep "
        "keeps the session running. Hardware support varies.",
        WS_CHILD | WS_VISIBLE,
        40, y, 390, 36, hwnd_parent, NULL, hInst, NULL);
    y += 43;

    CreateWindowEx(0, "BUTTON", "Auto-start with Windows",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        20, y, 240, 25, hwnd_parent, (HMENU)IDC_AUTO_START, hInst, NULL);
    if (tray->start_on_startup) {
        SendDlgItemMessage(hwnd_parent, IDC_AUTO_START, BM_SETCHECK, BST_CHECKED, 0);
    }
    y += 30;

    CreateWindowEx(0, "BUTTON", "Enable verbose logging",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        20, y, 240, 25, hwnd_parent, (HMENU)IDC_VERBOSE_LOGGING, hInst, NULL);
    if (tray->verbose) {
        SendDlgItemMessage(hwnd_parent, IDC_VERBOSE_LOGGING, BM_SETCHECK, BST_CHECKED, 0);
    }
    y += 30;

    CreateWindowEx(0, "BUTTON", "Check for updates on startup",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        20, y, 240, 25, hwnd_parent, (HMENU)IDC_CHECK_UPDATES_STARTUP, hInst, NULL);
    if (tray->check_updates_on_startup) {
        SendDlgItemMessage(hwnd_parent, IDC_CHECK_UPDATES_STARTUP, BM_SETCHECK, BST_CHECKED, 0);
    }
    y += 30;

    CreateWindowEx(0, "STATIC", "Auto check updates interval:",
        WS_CHILD | WS_VISIBLE,
        20, y, 180, 20, hwnd_parent, NULL, hInst, NULL);
    y += 22;

    HWND hCombo = CreateWindowEx(WS_EX_CLIENTEDGE, "COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        20, y, 200, 80, hwnd_parent, (HMENU)IDC_AUTO_CHECK_INTERVAL, hInst, NULL);
    if (hCombo) {
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"Never");
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"Daily");
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"Weekly");
        int idx = tray->auto_check_interval;
        if (idx < 0) idx = 0;
        if (idx > 2) idx = 2;
        SendMessage(hCombo, CB_SETCURSEL, (WPARAM)idx, 0);
    }
    y += 30;

    CreateWindowEx(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE,
        20, y, 410, 20, hwnd_parent, (HMENU)IDC_AUTO_CHECK_STATUS, hInst, NULL);
    y += 22;
    CreateWindowEx(0, "BUTTON", "Retry applied schedule",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        20, y, 200, 28, hwnd_parent, (HMENU)IDC_AUTO_CHECK_RETRY, hInst, NULL);
    refresh_auto_check_status(hwnd_parent, tray);
    y += 36;

    CreateWindowEx(0, "BUTTON", "Add nosleep to environment PATH",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        20, y, 260, 25, hwnd_parent, (HMENU)IDC_ADD_TO_PATH, hInst, NULL);
    if (tray->add_to_path) {
        SendDlgItemMessage(hwnd_parent, IDC_ADD_TO_PATH, BM_SETCHECK, BST_CHECKED, 0);
    }
}

// Create controls for the "Notifications" tab
static void create_notifications_tab(HWND hwnd_parent, NoSleepTray* tray) {
    HINSTANCE hInst = GetModuleHandle(NULL);

    // Create group listbox
    CreateWindowEx(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_HASSTRINGS,
        15, 10, 280, 200,
        hwnd_parent, (HMENU)IDC_NOTIFY_GROUP_LIST, hInst, NULL);

    // Configure button
    CreateWindowEx(0, "BUTTON", "Configure...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        305, 10, 120, 28, hwnd_parent, (HMENU)IDC_NOTIFY_CONFIGURE_GROUP, hInst, NULL);

    // Delete button
    CreateWindowEx(0, "BUTTON", "Delete",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        305, 45, 120, 28, hwnd_parent, (HMENU)IDC_NOTIFY_DEL_GROUP, hInst, NULL);

    // Set Active button
    CreateWindowEx(0, "BUTTON", "Set Active",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        305, 80, 120, 28, hwnd_parent, (HMENU)IDC_NOTIFY_SET_ACTIVE, hInst, NULL);

    // Add button
    CreateWindowEx(0, "BUTTON", "Add Group...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        305, 115, 120, 28, hwnd_parent, (HMENU)IDC_NOTIFY_ADD_GROUP, hInst, NULL);

    CreateWindowEx(0, "BUTTON", "Restore default...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        305, 150, 120, 28, hwnd_parent, (HMENU)IDC_NOTIFY_RESTORE_DEFAULT, hInst, NULL);

    // Help text for notification groups
    CreateWindowEx(0, "STATIC", 
        "Notification groups control which events show balloons.\n"
        "Select a group and click Configure... to edit it.\n"
        "Restore default... resets a built-in group's preset.\n"
        "Up to 20 groups; Add Group is disabled at the limit.\n"
        "Changes are saved immediately; Settings Cancel does\n"
        "not undo notification group changes.",
        WS_CHILD | WS_VISIBLE,
        15, 220, 410, 100, hwnd_parent, NULL, hInst, NULL);

    refresh_notification_group_list(hwnd_parent, tray);
}

// Reflect the selected group's available actions without replacing action guards.
static void update_notification_group_actions(HWND hwnd_parent, NoSleepTray* tray) {
    HWND hList = GetDlgItem(hwnd_parent, IDC_NOTIFY_GROUP_LIST);
    int selection = hList ? (int)SendMessage(hList, LB_GETCURSEL, 0, 0) : LB_ERR;
    int group_index = selection == LB_ERR
        ? LB_ERR
        : (int)SendMessage(hList, LB_GETITEMDATA, (WPARAM)selection, 0);
    bool valid = tray && group_index >= 0 && group_index < tray->notify_groups.count;

    EnableWindow(GetDlgItem(hwnd_parent, IDC_NOTIFY_ADD_GROUP),
        tray && tray->notify_groups.count < MAX_NOTIFY_GROUPS);
    EnableWindow(GetDlgItem(hwnd_parent, IDC_NOTIFY_CONFIGURE_GROUP), valid);
    EnableWindow(GetDlgItem(hwnd_parent, IDC_NOTIFY_DEL_GROUP),
        valid && !tray->notify_groups.groups[group_index].is_default);
    EnableWindow(GetDlgItem(hwnd_parent, IDC_NOTIFY_RESTORE_DEFAULT),
        valid && tray->notify_groups.groups[group_index].is_default);
    EnableWindow(GetDlgItem(hwnd_parent, IDC_NOTIFY_SET_ACTIVE),
        valid && group_index != tray->notify_groups.active_index);
}

// Refresh the notification group listbox
static void refresh_notification_group_list(HWND hwnd_parent, NoSleepTray* tray) {
    HWND hList = GetDlgItem(hwnd_parent, IDC_NOTIFY_GROUP_LIST);
    if (!hList || !tray) {
        update_notification_group_actions(hwnd_parent, tray);
        return;
    }

    int previous_count = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    int previous_row = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    int previous_group = previous_row == LB_ERR
        ? LB_ERR
        : (int)SendMessage(hList, LB_GETITEMDATA, (WPARAM)previous_row, 0);

    SendMessage(hList, LB_RESETCONTENT, 0, 0);

    HDC hdc = GetDC(hList);
    HFONT hFont = (HFONT)SendMessage(hList, WM_GETFONT, 0, 0);
    HGDIOBJ previous_font = hdc && hFont ? SelectObject(hdc, hFont) : NULL;
    int max_text_width = 0;

    for (int i = 0; i < tray->notify_groups.count; i++) {
        NotifyGroup* g = &tray->notify_groups.groups[i];
        char display[256];
        const char* marker = (i == tray->notify_groups.active_index) ? " [ACTIVE]" : "";
        snprintf(display, sizeof(display), "%s%s%s", 
                 g->name, marker, g->is_default ? " (default)" : "");
        int idx = (int)SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)display);
        SendMessage(hList, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);

        if (hdc) {
            SIZE text_size;
            if (GetTextExtentPoint32A(hdc, display, (int)strlen(display), &text_size) &&
                text_size.cx > max_text_width) {
                max_text_width = (int)text_size.cx;
            }
        }
    }

    if (previous_font) SelectObject(hdc, previous_font);
    if (hdc) ReleaseDC(hList, hdc);
    SendMessage(hList, LB_SETHORIZONTALEXTENT, (WPARAM)(max_text_width + 8), 0);

    // Keep the prior group selected when it survived the refresh. If the list
    // changed structurally or had no selection, show the active group instead.
    if (tray->notify_groups.count > 0) {
        int selection = tray->notify_groups.active_index;
        if (previous_count <= tray->notify_groups.count &&
            previous_group >= 0 && previous_group < tray->notify_groups.count) {
            selection = previous_group;
        }
        if (selection >= 0 && selection < tray->notify_groups.count) {
            SendMessage(hList, LB_SETCURSEL, (WPARAM)selection, 0);
        }
    }
    update_notification_group_actions(hwnd_parent, tray);
}

void tray_show_settings_dialog(NoSleepTray* tray) {
    if (!tray || tray_dialog_open) return;
    tray_dialog_open = true;

    HINSTANCE hInstance = GetModuleHandle(NULL);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = settings_dialog_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "NoSleepSettingsDialog";

    RegisterClass(&wc);

    DialogDpiContextFn set_dpi_context;
    HANDLE previous_context = enter_dialog_dpi_context(&set_dpi_context);
    RECT owner_bounds = {0};
    GetWindowRect(tray->hwnd, &owner_bounds);

    HWND hwndDlg = CreateWindowEx(
        0, "NoSleepSettingsDialog", "nosleep Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        owner_bounds.left, owner_bounds.top, 490, 440,
        tray->hwnd, NULL, hInstance, (LPVOID)tray
    );

    if (hwndDlg) {
        ShowWindow(hwndDlg, SW_SHOW);

        MSG msg;
        int message_result = 1;
        while (IsWindow(hwndDlg) && (message_result = GetMessage(&msg, NULL, 0, 0)) > 0) {
            if (!IsDialogMessage(hwndDlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }

        if (message_result <= 0) {
            if (IsWindow(hwndDlg)) DestroyWindow(hwndDlg);
            if (message_result == 0) PostQuitMessage((int)msg.wParam);
        }
    }

    if (previous_context) set_dpi_context(previous_context);

    UnregisterClass("NoSleepSettingsDialog", hInstance);
    tray_dialog_open = false;
}

// === Notification Group Edit Dialog ===
// Opens a popup to edit a notification group's name and event checkboxes

static bool handle_notify_group_bulk_toggle(HWND checkboxes[NOTIFY_EVENT_COUNT], UINT command_id) {
    UINT check_state;

    switch (command_id) {
        case IDC_NOTIFY_EVENT_SELECT_ALL:
            check_state = BST_CHECKED;
            break;
        case IDC_NOTIFY_EVENT_CLEAR_ALL:
            check_state = BST_UNCHECKED;
            break;
        default:
            return false;
    }

    for (int i = 0; i < NOTIFY_EVENT_COUNT; i++) {
        SendMessage(checkboxes[i], BM_SETCHECK, check_state, 0);
    }
    return true;
}

static unsigned int notify_group_event_mask_from_checkboxes(HWND checkboxes[NOTIFY_EVENT_COUNT]) {
    unsigned int mask = 0;

    for (int i = 0; i < NOTIFY_EVENT_COUNT; i++) {
        if (SendMessage(checkboxes[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= (1u << i);
        }
    }
    return mask;
}

static LRESULT CALLBACK notify_group_edit_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static NotifyGroupManager* edit_mgr = NULL;
    static int edit_index = -1;
    static HWND hCheckboxes[NOTIFY_EVENT_COUNT];
    static HWND hNameEdit = NULL;

    switch (msg) {
        case WM_CREATE:
        {
            // Use the global context which was set before CreateWindowEx
            edit_mgr = g_notify_edit_ctx.mgr;
            edit_index = g_notify_edit_ctx.group_index;
            
            HINSTANCE hInst = GetModuleHandle(NULL);

            int y = 15;

            // Group name label
            CreateWindowEx(0, "STATIC", "Group Name:",
                WS_CHILD | WS_VISIBLE,
                15, y, 100, 20, hwnd, NULL, hInst, NULL);

            // Group name edit
            hNameEdit = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                15, y + 22, 300, 25, hwnd, (HMENU)IDC_NOTIFY_GROUP_NAME, hInst, NULL);
            SendMessage(hNameEdit, EM_SETLIMITTEXT, (WPARAM)(MAX_GROUP_NAME - 1), 0);
            
            // Set name if editing existing group
            if (edit_index >= 0 && edit_mgr && edit_index < edit_mgr->count) {
                SetWindowText(hNameEdit, edit_mgr->groups[edit_index].name);
            } else {
                char name[MAX_GROUP_NAME] = "My Group";
                for (int suffix = 2; notify_groups_name_is_duplicate(edit_mgr, name, -1) &&
                     suffix <= MAX_NOTIFY_GROUPS + 1; suffix++) {
                    snprintf(name, sizeof(name), "My Group %d", suffix);
                }
                SetWindowText(hNameEdit, name);
            }
            
            y += 55;

            // Notification events checkboxes
            CreateWindowEx(0, "STATIC", "Show notifications for these events:",
                WS_CHILD | WS_VISIBLE,
                15, y, 300, 20, hwnd, NULL, hInst, NULL);
            y += 22;

            CreateWindowEx(0, "BUTTON", "Select All",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                15, y, 100, 24, hwnd, (HMENU)IDC_NOTIFY_EVENT_SELECT_ALL, hInst, NULL);

            CreateWindowEx(0, "BUTTON", "Clear All",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                125, y, 100, 24, hwnd, (HMENU)IDC_NOTIFY_EVENT_CLEAR_ALL, hInst, NULL);
            y += 30;

            unsigned int mask = 0;
            if (edit_index >= 0 && edit_mgr && edit_index < edit_mgr->count) {
                mask = edit_mgr->groups[edit_index].event_mask;
            } else {
                mask = 0xFFFFFFFF; // Default: all enabled for new groups
            }

            // Create checkboxes for each event
            for (int i = 0; i < NOTIFY_EVENT_COUNT; i++) {
                int row = i / 2;
                int col = i % 2;
                int cx = 15 + col * 220;
                int cy = y + row * 24;

                hCheckboxes[i] = CreateWindowEx(0, "BUTTON", NOTIFY_EVENT_NAMES[i],
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                    cx, cy, 210, 22, hwnd, (HMENU)(IDC_NOTIFY_EVENT_CHECK_START + i), hInst, NULL);
                
                if (mask & (1 << i)) {
                    SendMessage(hCheckboxes[i], BM_SETCHECK, BST_CHECKED, 0);
                }
            }

            // Calculate dialog size based on number of events
            int num_rows = (NOTIFY_EVENT_COUNT + 1) / 2;
            int dlg_height = y + num_rows * 24 + 60;

            // OK and Cancel buttons
            CreateWindowEx(0, "BUTTON", "OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                70, dlg_height - 40, 80, 28, hwnd, (HMENU)IDC_NOTIFY_GROUP_EDIT_OK, hInst, NULL);

            CreateWindowEx(0, "BUTTON", "Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                190, dlg_height - 40, 80, 28, hwnd, (HMENU)IDC_NOTIFY_GROUP_EDIT_CANCEL, hInst, NULL);

            if (!initialize_dialog_layout(hwnd, 460, dlg_height)) return -1;

            return 0;
        }

        case WM_COMMAND:
            if (handle_notify_group_bulk_toggle(hCheckboxes, LOWORD(wParam))) {
                break;
            }
            switch (LOWORD(wParam)) {
                case IDOK: // Enter from IsDialogMessage
                case IDC_NOTIFY_GROUP_EDIT_OK:
                {
                    if (!edit_mgr) break;

                    // Read group name
                    char name[MAX_GROUP_NAME] = "";
                    GetWindowText(hNameEdit, name, MAX_GROUP_NAME);
                    
                    // Validate name
                    if (notify_groups_name_is_blank(name)) {
                        MessageBox(hwnd, "Please enter a group name containing non-whitespace characters.",
                                   "Invalid Input", MB_OK | MB_ICONWARNING | MB_TOPMOST);
                        SetFocus(hNameEdit);
                        break;
                    }

                    if (notify_groups_name_is_duplicate(edit_mgr, name, edit_index)) {
                        MessageBox(hwnd, "A notification group with this name already exists. Please choose a different name.",
                                   "Duplicate Group Name", MB_OK | MB_ICONWARNING | MB_TOPMOST);
                        SetFocus(hNameEdit);
                        SendMessage(hNameEdit, EM_SETSEL, 0, -1);
                        break;
                    }

                    // Read event mask from checkboxes
                    unsigned int mask = notify_group_event_mask_from_checkboxes(hCheckboxes);

                    bool success = false;
                    bool group_change_valid = false;
                    NotifyGroupManager updated = *edit_mgr;

                    if (edit_index < 0) {
                        // Add new group
                        group_change_valid = notify_groups_add(&updated, name, mask) >= 0;
                    } else {
                        // Update existing group
                        group_change_valid = notify_groups_update(&updated, edit_index, name, mask);
                    }

                    if (group_change_valid) {
                        if (notify_groups_save(&updated)) {
                            *edit_mgr = updated;
                            success = true;
                        } else {
                            MessageBox(hwnd,
                                "Could not save the group to the registry. Please check registry access and try again.",
                                "nosleep - Notification group save failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
                            break;
                        }
                    }

                    if (success) {
                        DestroyWindow(hwnd);
                    } else {
                        MessageBox(hwnd, "Failed to save group. Max groups may be reached.",
                                   "Error", MB_OK | MB_ICONERROR | MB_TOPMOST);
                    }
                    break;
                }

                case IDCANCEL: // Escape from IsDialogMessage
                case IDC_NOTIFY_GROUP_EDIT_CANCEL:
                    DestroyWindow(hwnd);
                    break;
            }
            break;

        case WM_DPICHANGED:
            scale_dialog_layout(hwnd, HIWORD(wParam), (const RECT*)lParam);
            return 0;

        case WM_NCDESTROY:
            free_dialog_layout(hwnd);
            break;

        case WM_DESTROY:
            {
                HFONT hFont = (HFONT)GetWindowLongPtr(hwnd, GWLP_USERDATA);
                if (hFont) DeleteObject(hFont);
            }
            edit_mgr = NULL;
            edit_index = -1;
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void show_notify_group_edit_dialog(HWND hwnd_parent, NotifyGroupManager* mgr, int group_index) {
    if (!mgr) return;

    HINSTANCE hInstance = GetModuleHandle(NULL);
    BOOL owner_was_enabled = hwnd_parent && IsWindow(hwnd_parent) && IsWindowEnabled(hwnd_parent);
    if (owner_was_enabled) EnableWindow(hwnd_parent, FALSE);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = notify_group_edit_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "NoSleepNotifyGroupEditDialog";

    RegisterClass(&wc);

    const char* title = (group_index < 0) ? "New Notification Group" : "Edit Notification Group";

    // Set global context before creating window (WM_CREATE reads it)
    g_notify_edit_ctx.mgr = mgr;
    g_notify_edit_ctx.group_index = group_index;
    g_notify_edit_ctx.hwnd_parent = hwnd_parent;

    DialogDpiContextFn set_dpi_context;
    HANDLE previous_context = enter_dialog_dpi_context(&set_dpi_context);
    RECT owner_bounds = {0};
    GetWindowRect(hwnd_parent, &owner_bounds);

    HWND hwndDlg = CreateWindowEx(
        0, "NoSleepNotifyGroupEditDialog", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        owner_bounds.left, owner_bounds.top, 460, 400,
        hwnd_parent, NULL, hInstance, NULL
    );

    if (hwndDlg) {
        ShowWindow(hwndDlg, SW_SHOW);

        MSG msg;
        int message_result = 1;
        while (IsWindow(hwndDlg) && (message_result = GetMessage(&msg, NULL, 0, 0)) > 0) {
            if (!IsDialogMessage(hwndDlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }

        if (message_result <= 0) {
            if (IsWindow(hwndDlg)) DestroyWindow(hwndDlg);
            if (message_result == 0) PostQuitMessage((int)msg.wParam);
        }
    }

    if (owner_was_enabled && IsWindow(hwnd_parent)) {
        EnableWindow(hwnd_parent, TRUE);
        SetActiveWindow(hwnd_parent);
    }

    if (previous_context) set_dpi_context(previous_context);

    UnregisterClass("NoSleepNotifyGroupEditDialog", hInstance);
}

void tray_show_about_dialog(NoSleepTray* tray) {
    if (!tray || tray_dialog_open) return;
    tray_dialog_open = true;

    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASS wc = {0};
    wc.lpfnWndProc = about_dialog_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "NoSleepAboutDialog";
    RegisterClass(&wc);

    HWND hwndDlg = CreateWindowEx(
        0, "NoSleepAboutDialog", "About nosleep",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 380, 240,
        tray->hwnd, NULL, hInstance, (LPVOID)tray
    );

    if (hwndDlg) {
        ShowWindow(hwndDlg, SW_SHOW);

        MSG msg;
        int message_result = 1;
        while (IsWindow(hwndDlg) && (message_result = GetMessage(&msg, NULL, 0, 0)) > 0) {
            if (!IsDialogMessage(hwndDlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }

        if (message_result <= 0) {
            if (IsWindow(hwndDlg)) DestroyWindow(hwndDlg);
            if (message_result == 0) PostQuitMessage((int)msg.wParam);
        }
    }

    UnregisterClass("NoSleepAboutDialog", hInstance);
    tray_dialog_open = false;
}

// About dialog control IDs
#define IDC_ABOUT_CHECK_UPDATES 3001
#define IDC_ABOUT_OK            3002
#define IDC_ABOUT_UPDATE_STATUS 3003

// Called only on the tray UI thread. Keep the last manual result for the next
// time About is opened, while silent checks leave the user's feedback alone.
static void tray_set_manual_update_status(const char* status) {
    snprintf(manual_update_status, sizeof(manual_update_status), "%s",
             status ? status : "");
    if (about_dialog_hwnd) {
        SetDlgItemText(about_dialog_hwnd, IDC_ABOUT_UPDATE_STATUS,
                       manual_update_status);
    }
}

// Called only on the tray UI thread; the About handle is cleared on destruction.
static void tray_set_update_check_visible(NoSleepTray* tray, bool checking) {
    update_check_visible = checking;
    const char* label = checking ? "Checking for updates..." : "Check for Updates...";
    if (tray->hmenu) {
        ModifyMenu(tray->hmenu, IDM_CHECK_UPDATES,
            MF_BYCOMMAND | MF_STRING | (checking ? MF_GRAYED : MF_ENABLED),
            IDM_CHECK_UPDATES, label);
    }
    if (about_dialog_hwnd) {
        SetDlgItemText(about_dialog_hwnd, IDC_ABOUT_CHECK_UPDATES, label);
        EnableWindow(GetDlgItem(about_dialog_hwnd, IDC_ABOUT_CHECK_UPDATES), !checking);
    }
}

static LRESULT CALLBACK about_dialog_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static NoSleepTray* about_tray = NULL;

    switch (msg) {
        case WM_CREATE:
        {
            CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
            about_tray = (NoSleepTray*)cs->lpCreateParams;
            about_dialog_hwnd = hwnd;

            HINSTANCE hInst = GetModuleHandle(NULL);
            int y = 15;

            char line1[128];
            snprintf(line1, sizeof(line1), "nosleep v" CURRENT_VERSION);
            CreateWindowEx(0, "STATIC", line1,
                WS_CHILD | WS_VISIBLE, 20, y, 340, 20, hwnd, NULL, hInst, NULL);
            y += 25;

            CreateWindowEx(0, "STATIC", "A lightweight Windows utility that prevents system sleep",
                WS_CHILD | WS_VISIBLE, 20, y, 340, 20, hwnd, NULL, hInst, NULL);
            y += 22;

            CreateWindowEx(0, "STATIC", "License: GPL v3",
                WS_CHILD | WS_VISIBLE, 20, y, 340, 20, hwnd, NULL, hInst, NULL);
            y += 22;

            char build_info[128];
            snprintf(build_info, sizeof(build_info), "Built: %s %s", __DATE__, __TIME__);
            CreateWindowEx(0, "STATIC", build_info,
                WS_CHILD | WS_VISIBLE, 20, y, 340, 20, hwnd, NULL, hInst, NULL);
            y += 30;

            CreateWindowEx(0, "STATIC", manual_update_status,
                WS_CHILD | WS_VISIBLE, 20, y, 340, 30, hwnd,
                (HMENU)IDC_ABOUT_UPDATE_STATUS, hInst, NULL);
            y += 34;

            CreateWindowEx(0, "BUTTON",
                update_check_visible ? "Checking for updates..." : "Check for Updates...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON |
                    (update_check_visible ? WS_DISABLED : 0),
                20, y, 160, 28, hwnd, (HMENU)IDC_ABOUT_CHECK_UPDATES, hInst, NULL);

            CreateWindowEx(0, "BUTTON", "OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                200, y, 140, 28, hwnd, (HMENU)IDC_ABOUT_OK, hInst, NULL);

            // Apply high-quality ClearType font to all dialog controls
            HFONT aboutFont = create_dialog_font(14);
            if (aboutFont) {
                EnumChildWindows(hwnd, set_child_font_proc, (LPARAM)aboutFont);
                // Store font handle on window for cleanup in WM_DESTROY
                SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)aboutFont);
            }

            RECT rc;
            GetWindowRect(hwnd, &rc);
            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);
            int x = (screenWidth - (rc.right - rc.left)) / 2;
            int yy = (screenHeight - (rc.bottom - rc.top)) / 2;
            SetWindowPos(hwnd, NULL, x, yy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_ABOUT_CHECK_UPDATES:
                    if (update_check_visible) return TRUE;
                    tray_check_for_updates(about_tray, false);
                    return TRUE;
                case IDCANCEL: // Escape from IsDialogMessage
                case IDC_ABOUT_OK:
                    DestroyWindow(hwnd);
                    return TRUE;
            }
            break;

        case WM_DESTROY:
            {
                // Clean up GDI font stored via SetWindowLongPtr
                HFONT hFont = (HFONT)GetWindowLongPtr(hwnd, GWLP_USERDATA);
                if (hFont) {
                    DeleteObject(hFont);
                }
            }
            about_dialog_hwnd = NULL;
            about_tray = NULL;
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// This worker performs only the network request. It hands the result to the
// tray window, which remains responsible for notifications and dialogs.
static DWORD WINAPI tray_update_check_worker(LPVOID parameter) {
    TrayUpdateCheckTask* task = (TrayUpdateCheckTask*)parameter;
    task->check_succeeded = updater_check(&task->info, NULL);
    MEMORY_BARRIER();
    while (!PostMessage(task->hwnd, WM_TRAY_UPDATE_CHECK_COMPLETE, 0,
                        (LPARAM)task)) {
        if (!IsWindow(task->hwnd)) {
            DEBUG_LOG("tray_update_check_worker: tray window closed before result delivery");
            return 0;
        }
        Sleep(10);
    }
    return 0;
}

// Only explicit user actions call this prompt, which retains the release-notes choice.
static void tray_prompt_available_update(NoSleepTray* tray, UpdateInfo* info) {
    // Ask user if they want to download
    bool want_download = updater_show_prompt_dialog(tray->hwnd, info);
    if (!want_download) {
        return;
    }

    // Get current executable path
    wchar_t* exe_path = get_exe_path_w();
    if (!exe_path) {
        MessageBox(tray->hwnd, "Could not determine executable path.",
                   "Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return;
    }

    // Download and install
    bool update_started = updater_download_and_install(info, exe_path, tray->hwnd);
    free(exe_path);

    if (update_started) {
        // Exit the application so the update can complete
        PostMessage(tray->hwnd, WM_CLOSE, 0, 0);
    }

}

static void tray_process_update_check_result(NoSleepTray* tray, bool silent,
                                              bool check_ok, UpdateInfo* info) {
    save_last_update_check_time();

    if (!check_ok) {
        if (!silent) {
            tray_set_manual_update_status("Check failed");
            tray_show_notification(tray, NOTIFY_EVENT_UPDATE_CHECK_FAILED,
                "Update Check Failed", "Could not check for updates. Check your internet connection.", false);
        }
        goto update_check_done;
    }

    if (!info || !info->update_available ||
        updater_compare_versions(info->latest_version, CURRENT_VERSION) <= 0) {
        // A successful check with no newer update clears the cached result.
        memset(&tray->available_update, 0, sizeof(tray->available_update));
        EnableMenuItem(tray->hmenu, IDM_REVIEW_UPDATE, MF_BYCOMMAND | MF_GRAYED);
        if (!silent) {
            char status[128];
            snprintf(status, sizeof(status), "Latest version: v%s", CURRENT_VERSION);
            tray_set_manual_update_status(status);
            tray_show_notification(tray, NOTIFY_EVENT_UPDATE_CHECK_COMPLETED,
                "No Updates",
                "You are running the latest version (v" CURRENT_VERSION ")", false);
        }
        goto update_check_done;
    }

    // Keep the prior version until the new result's notification decision is made.
    bool notify_update_available = !silent || !tray->available_update.update_available ||
        updater_compare_versions(info->latest_version,
                                 tray->available_update.latest_version) > 0;

    // Copy the worker result before its stack snapshot goes out of scope.
    tray->available_update = *info;
    EnableMenuItem(tray->hmenu, IDM_REVIEW_UPDATE, MF_BYCOMMAND | MF_ENABLED);

    // The menu remains available even when notifications are suppressed.
    if (notify_update_available) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Version %s is available! Right-click the tray icon and choose Review Available Update.", info->latest_version);
        tray_show_notification(tray, NOTIFY_EVENT_UPDATE_AVAILABLE, "Update Available", msg, false);
    }

    if (!silent) {
        char status[128];
        snprintf(status, sizeof(status), "Version %s is available",
                 info->latest_version);
        tray_set_manual_update_status(status);
        UpdateInfo available = tray->available_update;
        tray_prompt_available_update(tray, &available);
    }

update_check_done:
    tray_update_check_end();
    tray_set_update_check_visible(tray, false);
}

static void tray_handle_update_check_complete(NoSleepTray* tray,
                                               TrayUpdateCheckTask* task) {
    if (!tray || !task || tray->update_check_task != task) return;

    // The worker publishes its result before posting this message.
    MEMORY_BARRIER();
    if (task->thread) {
        if (WaitForSingleObject(task->thread, INFINITE) != WAIT_OBJECT_0) {
            DEBUG_LOG("tray_handle_update_check_complete: waiting for update worker failed");
            return;
        }
        CloseHandle(task->thread);
        task->thread = NULL;
    }

    bool silent = task->silent;
    bool check_ok = task->check_succeeded;
    UpdateInfo info = task->info;
    tray->update_check_task = NULL;
    free(task);

    tray_process_update_check_result(tray, silent, check_ok, &info);
}

// Update checking - performs network access on a worker and UI handling on the tray thread.
void tray_check_for_updates(NoSleepTray* tray, bool silent) {
    if (!tray || tray->update_check_task || !tray_update_check_begin()) return;

    tray_set_update_check_visible(tray, true);
    DEBUG_LOG("tray_check_for_updates: starting asynchronous check");

    TrayUpdateCheckTask* task = (TrayUpdateCheckTask*)calloc(1, sizeof(TrayUpdateCheckTask));
    if (!task) {
        UpdateInfo empty_info = {0};
        tray_process_update_check_result(tray, silent, false, &empty_info);
        return;
    }

    task->hwnd = tray->hwnd;
    task->silent = silent;
    tray->update_check_task = task;
    task->thread = CreateThread(NULL, 0, tray_update_check_worker, task, 0,
                                &task->thread_id);
    if (!task->thread) {
        tray->update_check_task = NULL;
        free(task);
        UpdateInfo empty_info = {0};
        tray_process_update_check_result(tray, silent, false, &empty_info);
    }
}

static void tray_apply_auto_check_interval(NoSleepTray* tray, int interval) {
    if (!tray || interval == CB_ERR) return;
    if (tray->auto_check_interval == interval &&
        (interval == 0 || tray->update_timer_id != 0)) return;

    tray->auto_check_interval = interval;
    tray_setup_update_timer(tray);
}

static void tray_setup_update_timer(NoSleepTray* tray) {
    if (!tray || !tray->hwnd) return;

    if (tray->update_timer_id) {
        KillTimer(tray->hwnd, tray->update_timer_id);
        tray->update_timer_id = 0;
    }

    if (tray->auto_check_interval == 0) return;

    UINT interval_ms;
    if (tray->auto_check_interval == 1) {
        interval_ms = 24 * 60 * 60 * 1000;
    } else {
        interval_ms = 7 * 24 * 60 * 60 * 1000;
    }

    tray->update_timer_id = SetTimer(tray->hwnd, 1002, interval_ms, NULL);
    if (!tray->update_timer_id) {
        tray_show_notification(tray, NOTIFY_EVENT_UPDATE_CHECK_FAILED,
            "Automatic Updates Not Scheduled",
            "Could not schedule automatic update checks. Open Settings and use Retry applied schedule to retry.", false);
    }
}



LRESULT CALLBACK tray_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    NoSleepTray* tray = NULL;
    
    if (msg == WM_CREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        tray = (NoSleepTray*)cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)tray);
        return 0;
    }
    
    tray = (NoSleepTray*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    if (tray_handle_taskbar_created(tray, msg)) {
        return 0;
    }
    
    // Check for tray icon message (either registered or default)
    if (tray && (msg == tray->uTrayMessage || msg == TRAY_ICON_MESSAGE_ID)) {
        const char* msg_name = "unknown";
        if (lParam == WM_MOUSEMOVE) msg_name = "WM_MOUSEMOVE";
        else if (lParam == WM_LBUTTONDOWN) msg_name = "WM_LBUTTONDOWN";
        else if (lParam == WM_LBUTTONUP) msg_name = "WM_LBUTTONUP";
        else if (lParam == WM_LBUTTONDBLCLK) msg_name = "WM_LBUTTONDBLCLK";
        else if (lParam == WM_RBUTTONDOWN) msg_name = "WM_RBUTTONDOWN";
        else if (lParam == WM_RBUTTONUP) msg_name = "WM_RBUTTONUP";
        else if (lParam == WM_RBUTTONDBLCLK) msg_name = "WM_RBUTTONDBLCLK";
        else if (lParam == WM_MBUTTONDOWN) msg_name = "WM_MBUTTONDOWN";
        else if (lParam == WM_MBUTTONUP) msg_name = "WM_MBUTTONUP";
        else if (lParam == WM_MBUTTONDBLCLK) msg_name = "WM_MBUTTONDBLCLK";
        else if (lParam == NIN_SELECT) msg_name = "NIN_SELECT";
        else if (lParam == NIN_KEYSELECT) msg_name = "NIN_KEYSELECT";
        else if (lParam == NIN_BALLOONSHOW) msg_name = "NIN_BALLOONSHOW";
        else if (lParam == NIN_BALLOONHIDE) msg_name = "NIN_BALLOONHIDE";
        else if (lParam == NIN_BALLOONTIMEOUT) msg_name = "NIN_BALLOONTIMEOUT";
        else if (lParam == NIN_BALLOONUSERCLICK) msg_name = "NIN_BALLOONUSERCLICK";
        
        // Only print debug info if NOSLEEP_DEBUG environment variable is set
        DEBUG_LOG("tray_window_proc: Tray message received (msg=0x%X) wParam=%lld, lParam=%lld (%s)", msg, wParam, lParam, msg_name);
        DEBUG_LOG("tray_window_proc: lParam low word=0x%04X, high word=0x%04X", LOWORD(lParam), HIWORD(lParam));
        
        // Handle right-click (both legacy mouse message and notification code)
        if (lParam == WM_RBUTTONUP || lParam == NIN_KEYSELECT) {
            // Show context menu
            DEBUG_LOG("tray_window_proc: Right-click detected, showing menu");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd); // Required for menu to disappear properly
            // Enable/disable Stop menu item based on running state
            if (tray_has_stop_work(tray)) {
                EnableMenuItem(tray->hmenu, IDM_STOP, MF_BYCOMMAND | MF_ENABLED);
            } else {
                EnableMenuItem(tray->hmenu, IDM_STOP, MF_BYCOMMAND | MF_GRAYED);
            }
            TrackPopupMenu(tray->hmenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            PostMessage(hwnd, WM_NULL, 0, 0); // Send dummy message to make menu disappear
        }
        return 0;
    }
    
    switch (msg) {

        case WM_TRAY_UPDATE_CHECK_COMPLETE:
            if (tray) {
                tray_handle_update_check_complete(tray,
                    (TrayUpdateCheckTask*)lParam);
            }
            break;
            
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_START_30MIN:
                    tray_start_nosleep(tray, 30);
                    break;
                case IDM_START_1HOUR:
                    tray_start_nosleep(tray, 60);
                    break;
                case IDM_START_2HOURS:
                    tray_start_nosleep(tray, 120);
                    break;
                case IDM_START_CUSTOM: {
                    int minutes = tray_show_custom_dialog(tray);
                    if (minutes > 0) {
                        tray_start_nosleep(tray, minutes);
                    }
                    break;
                }
                case IDM_START_INDEFINITE:
                    tray_start_nosleep(tray, 0); // 0 = indefinite
                    break;
                case IDM_STOP:
                    {
                        AcquireSRWLockExclusive(&tray->delayed_action_lock);
                        bool sleep_timer_active = tray->sleep_timer != NULL;
                        bool shutdown_timer_active = tray->shutdown_timer != NULL;
                        ReleaseSRWLockExclusive(&tray->delayed_action_lock);
                        DEBUG_LOG("IDM_STOP: is_running=%s, delayed_sleep_countdown_active=%s, sleep_timer=%s, shutdown_timer=%s",
                                ATOMIC_LOAD_BOOL(&tray->is_running) ? "true" : "false",
                                ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active) ? "true" : "false",
                                sleep_timer_active ? "active" : "inactive",
                                shutdown_timer_active ? "active" : "inactive");
                        tray_stop_nosleep(tray, false, false); // show notification when manually stopping
                    }
                    break;
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    break;
                case IDM_ABOUT:
                    tray_show_about_dialog(tray);
                    break;
                case IDM_SETTINGS:
                    tray_show_settings_dialog(tray);
                    break;
                case IDM_REVIEW_UPDATE:
                    if (tray->available_update.update_available && tray_update_check_begin()) {
                        UpdateInfo available = tray->available_update;
                        tray_prompt_available_update(tray, &available);
                        tray_update_check_end();
                    }
                    break;
                case IDM_CHECK_UPDATES:
                    tray_check_for_updates(tray, false);
                    break;
                case IDM_TOGGLE_SLEEP_AFTER_TIMEOUT:
                    // Backward compatibility: toggle between SLEEP and NONE
                    if (tray->session_finished_action == SESSION_FINISHED_SLEEP) {
                        tray->session_finished_action = SESSION_FINISHED_NONE;
                    } else {
                        tray->session_finished_action = SESSION_FINISHED_SLEEP;
                    }
                    tray->sleep_after_timeout = (tray->session_finished_action == SESSION_FINISHED_SLEEP);
                    // Update menu checkmarks
                    if (tray->hmenu) {
                        tray_update_session_finished_menu(tray);
                    }
                    break;
                case IDM_SESSION_FINISHED_NONE:
                    tray->session_finished_action = SESSION_FINISHED_NONE;
                    tray->sleep_after_timeout = false;
                    tray_update_session_finished_menu(tray);
                    tray_save_settings_with_warning(hwnd, tray);
                    break;
                case IDM_SESSION_FINISHED_SHUTDOWN:
                    tray->session_finished_action = SESSION_FINISHED_SHUTDOWN;
                    tray->sleep_after_timeout = false;
                    tray_update_session_finished_menu(tray);
                    tray_save_settings_with_warning(hwnd, tray);
                    break;
                case IDM_SESSION_FINISHED_SHUTDOWN_GRACEFUL:
                    tray->session_finished_action = SESSION_FINISHED_SHUTDOWN_GRACEFUL;
                    tray->sleep_after_timeout = false;
                    tray_update_session_finished_menu(tray);
                    tray_save_settings_with_warning(hwnd, tray);
                    break;
                case IDM_SESSION_FINISHED_SLEEP:
                    tray->session_finished_action = SESSION_FINISHED_SLEEP;
                    tray->sleep_after_timeout = true;
                    tray_update_session_finished_menu(tray);
                    tray_save_settings_with_warning(hwnd, tray);
                    break;
            }
            break;
            
        case WM_TIMER:
            if (wParam == TIMER_ID_AUTO_START) {
                KillTimer(hwnd, TIMER_ID_AUTO_START);
                DEBUG_PRINT("Auto-starting 1 minute nosleep (testing)\n");
                tray_start_nosleep(tray, 1); // 1 minute for testing
            } else if (wParam == 1002) {
                // Periodic update check timer
                if (tray) {
                    tray_check_for_updates(tray, true);
                }
            }
            break;
            
        case WM_DESTROY:
            // Remove tray icon
            if (tray) {
                AcquireSRWLockExclusive(&tray->tray_icon_lock);
                Shell_NotifyIcon(NIM_DELETE, &tray->nid);
                ReleaseSRWLockExclusive(&tray->tray_icon_lock);
            }
            PostQuitMessage(0);
            break;
            
        case WM_POWERBROADCAST:
            {
                DEBUG_LOG("WM_POWERBROADCAST: wParam=0x%llX", wParam);
                
                switch (wParam) {
                    case PBT_APMSUSPEND:
                        // System is about to enter sleep
                        DEBUG_LOG("System entering sleep, stopping all timers");
                        
                        // Do not wait for action workers here: a sleep action can
                        // be generating this broadcast while waiting for it to return.
                        AcquireSRWLockExclusive(&tray->delayed_action_lock);
                        tray_wait_for_delayed_countdown_start(tray);
                        SetEvent(tray->sleep_stop_event);
                        SetEvent(tray->shutdown_stop_event);
                        ReleaseSRWLockExclusive(&tray->delayed_action_lock);

                        // Stop any countdown that was already active.
                        if (ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active)) {
                            tray_stop_countdown(tray);
                        }
                        
                        // Show notification
                        tray_show_notification(tray, NOTIFY_EVENT_SLEEP_DETECTED,
                            "Sleep Detected", "System sleep cancelled all pending actions", false);
                        break;
                        
                    case PBT_APMRESUMESUSPEND:
                        // System resumed from sleep
                        DEBUG_LOG("System resumed from sleep");
                        break;
                }
                
                // Return TRUE to indicate we processed the message
                return TRUE;
            }
            break;
            
        case WM_CONTEXTMENU:
            {
                DEBUG_LOG("tray_window_proc: WM_CONTEXTMENU received");
                if (tray && tray->hmenu) {
                    POINT pt;
                    GetCursorPos(&pt);
                    SetForegroundWindow(hwnd);
                    // Enable/disable Stop menu item based on running state
                    if (tray_has_stop_work(tray)) {
                        EnableMenuItem(tray->hmenu, IDM_STOP, MF_BYCOMMAND | MF_ENABLED);
                    } else {
                        EnableMenuItem(tray->hmenu, IDM_STOP, MF_BYCOMMAND | MF_GRAYED);
                    }
                    TrackPopupMenu(tray->hmenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                    PostMessage(hwnd, WM_NULL, 0, 0);
                }
            }
            break;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    
    return 0;
}
