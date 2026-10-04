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

// Window class name for tray icon
static const char* TRAY_WINDOW_CLASS = "NoSleepTrayWindowClass";

// Timer ID for auto-start debug feature
#define TIMER_ID_AUTO_START 1000

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
static void load_gray_and_color_icons(NoSleepTray* tray);
static DWORD WINAPI delayed_sleep_thread(LPVOID lpParam);
static DWORD WINAPI delayed_shutdown_thread(LPVOID lpParam);
static void trigger_system_sleep(NoSleepTray* tray);
static void trigger_system_shutdown(NoSleepTray* tray);
static char* get_exe_path(void);
static bool is_startup_enabled(void);
static bool set_startup_registry(bool enable);
static bool should_check_for_updates(void);
static void tray_setup_update_timer(NoSleepTray* tray);
static void tray_apply_auto_check_interval(NoSleepTray* tray, int interval);
static LRESULT CALLBACK about_dialog_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static bool add_app_to_path(void);
static bool remove_app_from_path(void);
static bool apply_path_preference(bool add_to_path);
static int str_icmp_n(const char* a, const char* b, size_t n);
static char* get_exe_dir(void);
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
    
    // Register unique tray message
    tray->uTrayMessage = RegisterWindowMessage("nosleep_tray_message");
    if (tray->uTrayMessage == 0) {
        DEBUG_PRINT("tray_init: RegisterWindowMessage failed, using default TRAY_ICON_MESSAGE_ID\n");
        tray->uTrayMessage = TRAY_ICON_MESSAGE_ID;
    }
    DEBUG_PRINT("tray_init: Registered tray message ID: %u (0x%X)\n", tray->uTrayMessage, tray->uTrayMessage);
    
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
    if (!Shell_NotifyIcon(NIM_ADD, &tray->nid)) {
        DEBUG_PRINT("tray_init: Shell_NotifyIcon failed with error %lu\n", GetLastError());
        DestroyWindow(tray->hwnd);
        return false;
    }
    DEBUG_PRINT("tray_init: Tray icon added successfully\n");
    DEBUG_PRINT("tray_init: Icon added with hWnd=%p, uID=%u, uCallbackMessage=%u\n", tray->nid.hWnd, tray->nid.uID, tray->nid.uCallbackMessage);
    
    // Set tray icon version to legacy version (NOTIFYICON_VERSION = 3) for mouse messages
    tray->nid.uVersion = 3; // NOTIFYICON_VERSION
    if (!Shell_NotifyIcon(NIM_SETVERSION, &tray->nid)) {
        DEBUG_PRINT("tray_init: NIM_SETVERSION failed with error %lu\n", GetLastError());
    } else {
        DEBUG_PRINT("tray_init: Tray icon version set to %u (legacy mouse messages)\n", tray->nid.uVersion);
    }
    
    // Read startup state from registry
    tray->start_on_startup = is_startup_enabled();

    // Load settings from registry
    tray_load_settings(tray);

    // Apply the saved PATH preference on every startup. A failed update is
    // retried next time because the preference records the desired state.
    if (!apply_path_preference(tray->add_to_path)) {
        DEBUG_PRINT("tray_init: failed to apply PATH preference; it will be retried on next launch\n");
    }

    // Initialize notification groups and migrate old settings
    // Migrate the old mode only if no groups were loaded from the registry.
    notify_groups_init(&tray->notify_groups, tray->notification_mode);

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
    
    // Convert to grayscale by iterating pixels
    DWORD* pixels = (DWORD*)pBits;
    for (int i = 0; i < width * height; i++) {
        DWORD pixel = pixels[i];
        BYTE r = GetRValue(pixel);
        BYTE g = GetGValue(pixel);
        BYTE b = GetBValue(pixel);
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
    tray->hIconActive = load_icon_from_resource(MAKEINTRESOURCE(IDI_APPICON), 32, 32);
    
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
    // Create a 32x32 color icon with solid background
    const int width = 32;
    const int height = 32;
    
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
        // Coordinates scaled to 32x32 (37.5% area like Python version)
        // Top horizontal line (10,10 to 22,10)
        for (int x = 10; x <= 22; x++) {
            int y = 10;
            pixels[y * width + x] = 0xFFFFFFFF; // White opaque
        }
        // Diagonal line (22,10 to 10,22)
        for (int d = 0; d <= 12; d++) {
            int x = 22 - d;
            int y = 10 + d;
            pixels[y * width + x] = 0xFFFFFFFF;
        }
        // Bottom horizontal line (10,22 to 22,22)
        for (int x = 10; x <= 22; x++) {
            int y = 22;
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
    const int width = 32;
    const int height = 32;
    
    // Create device contexts and bitmaps
    HDC hdc = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdc);
    HDC hdcMask = CreateCompatibleDC(hdc);
    
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
    // Scale to 32x32: starting font = 80 * (32/128) = 20
    // Max dimensions = 110 * (32/128) = 27.5 -> use 27
    const int startFontSize = 20;
    const int maxDim = 27;
    
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
    
    // For each pixel, set alpha based on luminance (max component) where text is drawn
    // This preserves anti-aliasing edges by using alpha blending
    for (int i = 0; i < width * height; i++) {
        DWORD color = pixels[i];
        BYTE r = (color >> 16) & 0xFF;
        BYTE g = (color >> 8) & 0xFF;
        BYTE b = color & 0xFF;
        BYTE luminance = (BYTE)((r + g + b) / 3); // Simple average for grayscale
        if (luminance > 0) {
            // Text pixel (white with anti-aliasing). Set alpha to luminance.
            // Keep original RGB (may be gray for anti-aliased edges) and set alpha.
            BYTE alpha = luminance;
            pixels[i] = ((DWORD)alpha << 24) | (color & 0x00FFFFFF); // Preserve RGB with alpha
        }
        // else alpha remains 0 (transparent)
    }
    
    // Update mask bitmap: draw text as black (opaque) on white (transparent) background
    HFONT oldFontMask = NULL;
    if (hFont) {
        oldFontMask = (HFONT)SelectObject(hdcMask, hFont);
        SetTextColor(hdcMask, RGB(0, 0, 0)); // Black (opaque in mask)
        SetBkMode(hdcMask, TRANSPARENT);
        SetTextAlign(hdcMask, TA_LEFT | TA_TOP); // Match color bitmap alignment
        TextOutW(hdcMask, textX, textY, text, (int)wcslen(text));
    }
    
    // Clean up GDI objects
    if (oldFont) {
        SelectObject(hdcMem, oldFont);
    }
    if (oldFontMask) {
        SelectObject(hdcMask, oldFontMask);
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
    
    const int width = 32;
    const int height = 32;
    
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
        
        // Alternative: create a 32x32 icon with alpha=0
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
    tray->hmenu = CreatePopupMenu();
    DEBUG_LOG("tray_create_menu: CreatePopupMenu returned %p", tray->hmenu);
    if (!tray->hmenu) {
        DEBUG_LOG("tray_create_menu: Failed to create menu (error %lu)", GetLastError());
        return;
    }
    
    // Duration submenu
    HMENU hSubMenu = CreatePopupMenu();
    DEBUG_LOG("tray_create_menu: CreatePopupMenu submenu returned %p", hSubMenu);
    if (!hSubMenu) {
        DEBUG_LOG("tray_create_menu: Failed to create submenu (error %lu)", GetLastError());
        DestroyMenu(tray->hmenu);
        tray->hmenu = NULL;
        return;
    }
    
    AppendMenu(hSubMenu, MF_STRING, IDM_START_30MIN, "30 minutes");
    AppendMenu(hSubMenu, MF_STRING, IDM_START_1HOUR, "1 hour");
    AppendMenu(hSubMenu, MF_STRING, IDM_START_2HOURS, "2 hours");
    AppendMenu(hSubMenu, MF_STRING, IDM_START_CUSTOM, "Custom...");
    AppendMenu(hSubMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hSubMenu, MF_STRING, IDM_START_INDEFINITE, "Indefinite");
    
    // When finished submenu
    HMENU hSessionFinishedMenu = CreatePopupMenu();
    if (hSessionFinishedMenu) {
        AppendMenu(hSessionFinishedMenu, MF_STRING | (tray->session_finished_action == SESSION_FINISHED_NONE ? MF_CHECKED : MF_UNCHECKED), IDM_SESSION_FINISHED_NONE, "None");
        AppendMenu(hSessionFinishedMenu, MF_STRING | (tray->session_finished_action == SESSION_FINISHED_SHUTDOWN ? MF_CHECKED : MF_UNCHECKED), IDM_SESSION_FINISHED_SHUTDOWN, "Shutdown");
        AppendMenu(hSessionFinishedMenu, MF_STRING | (tray->session_finished_action == SESSION_FINISHED_SLEEP ? MF_CHECKED : MF_UNCHECKED), IDM_SESSION_FINISHED_SLEEP, "Sleep");
    }
    
    // Create main menu item text based on current selection
    char finished_text[64];
    switch (tray->session_finished_action) {
        case SESSION_FINISHED_NONE:
            snprintf(finished_text, sizeof(finished_text), "When finished(None)");
            break;
        case SESSION_FINISHED_SHUTDOWN:
            snprintf(finished_text, sizeof(finished_text), "When finished(Shutdown)");
            break;
        case SESSION_FINISHED_SLEEP:
            snprintf(finished_text, sizeof(finished_text), "When finished(Sleep)");
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
    AppendMenu(tray->hmenu, MF_STRING | MF_POPUP, (UINT_PTR)hSessionFinishedMenu, finished_text);
    AppendMenu(tray->hmenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(tray->hmenu, MF_STRING | MF_POPUP, (UINT_PTR)hSubMenu, "Set Duration");
    AppendMenu(tray->hmenu, MF_STRING, IDM_STOP, "Stop");
    AppendMenu(tray->hmenu, MF_SEPARATOR, 0, NULL);
    // Group: Settings, Check for Updates, About
    AppendMenu(tray->hmenu, MF_STRING, IDM_SETTINGS, "Settings...");
    AppendMenu(tray->hmenu, MF_STRING, IDM_CHECK_UPDATES, "Check for Updates...");
    AppendMenu(tray->hmenu, MF_STRING, IDM_ABOUT, "About");
    AppendMenu(tray->hmenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(tray->hmenu, MF_STRING, IDM_EXIT, "Exit");
    DEBUG_LOG("tray_create_menu: Menu created successfully");
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
            ReleaseSRWLockExclusive(&tray->delayed_action_lock);
            tray_stop_nosleep(tray, false, true); // suppress notification

            AcquireSRWLockExclusive(&tray->delayed_action_lock);
            tray->starting_nosleep = false;
            ReleaseSRWLockExclusive(&tray->delayed_action_lock);
            
            char error_msg[256];
            sprintf(error_msg, "Failed to create timer thread. Error code: %lu", GetLastError());
            tray_show_notification(tray, NOTIFY_EVENT_ERROR, "Error", error_msg, true);
            return;
        }
    }

    tray->starting_nosleep = false;
    ReleaseSRWLockExclusive(&tray->delayed_action_lock);
    
    // Update stop menu item text
    tray_update_stop_menu_item(tray);
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
    if (is_nosleep_thread) {
        ATOMIC_STORE_BOOL(&tray->core_init_failed, true);
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
    SessionFinishedAction countdown_action = tray->countdown_action;
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
            if (WaitForSingleObject(tray->nosleep_thread, 2000) == WAIT_OBJECT_0) {
                CloseHandle(tray->nosleep_thread);
                tray->nosleep_thread = NULL;
            }
        }
    }
    
    // Cancel and wait for sleep timer thread if active
    if (tray->sleep_timer) {
        // Signal cancellation
        SetEvent(tray->sleep_stop_event);
        
        DWORD current_thread_id = GetCurrentThreadId();
        DWORD sleep_timer_thread_id = GetThreadId(tray->sleep_timer);
        if (current_thread_id != sleep_timer_thread_id) {
            WaitForSingleObject(tray->sleep_timer, INFINITE);
        }
        CloseHandle(tray->sleep_timer);
        tray->sleep_timer = NULL;
        
        // Reset the event for future use
        ResetEvent(tray->sleep_stop_event);
    }
    
    // Cancel and wait for shutdown timer thread if active
    if (tray->shutdown_timer) {
        // Signal cancellation
        SetEvent(tray->shutdown_stop_event);
        
        DWORD current_thread_id = GetCurrentThreadId();
        DWORD shutdown_timer_thread_id = GetThreadId(tray->shutdown_timer);
        if (current_thread_id != shutdown_timer_thread_id) {
            WaitForSingleObject(tray->shutdown_timer, INFINITE);
        }
        CloseHandle(tray->shutdown_timer);
        tray->shutdown_timer = NULL;
        
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
    DEBUG_LOG("tray_stop_nosleep: timer_expired=%s, duration_expired=%s, was_countdown_active=%s", timer_expired ? "true" : "false", ATOMIC_LOAD_BOOL(&tray->duration_expired) ? "true" : "false", was_countdown_active ? "true" : "false");
    
    // Determine which notification to show
    if (!suppress_notification) {
        if (was_countdown_active) {
            // Countdown was cancelled
            DEBUG_LOG("tray_stop_nosleep: showing countdown cancellation notification");
            if (countdown_action == SESSION_FINISHED_SHUTDOWN) {
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
            if (tray->session_finished_action != SESSION_FINISHED_NONE) {
                // Do not honor a follow-up action until the NoSleep core is known to exist.
                for (;;) {
                    AcquireSRWLockExclusive(&tray->delayed_action_lock);
                    bool is_current_session = (tray->timer_thread_id == timer_thread_id);
                    bool is_running = ATOMIC_LOAD_BOOL(&tray->is_running);
                    bool core_init_failed = ATOMIC_LOAD_BOOL(&tray->core_init_failed);
                    bool core_init_succeeded = ATOMIC_LOAD_BOOL(&tray->core_init_succeeded);
                    bool session_starting = tray->starting_nosleep;
                    ReleaseSRWLockExclusive(&tray->delayed_action_lock);

                    if (!is_current_session || !is_running || core_init_failed || session_starting) {
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
            bool suppress_notification = (tray->session_finished_action != SESSION_FINISHED_NONE);
            if (!tray_stop_nosleep_for_session(
                    tray, timer_thread_id, true, suppress_notification)) {
                return 0;
            }
            
            // Calculate elapsed time for notifications from the monotonic tick.
            ULONGLONG elapsed_seconds = get_elapsed_milliseconds(tray->start_tick64) / 1000;
            
            int hours = (int)(elapsed_seconds / 3600);
            int minutes = (int)((elapsed_seconds % 3600) / 60);
            int seconds = (int)(elapsed_seconds % 60);
            
            char duration_message[256];
            if (hours > 0) {
                sprintf(duration_message, "Sleep prevention stopped\nDuration: %dh %dm", hours, minutes);
            } else {
                sprintf(duration_message, "Sleep prevention stopped\nDuration: %dm %ds", minutes, seconds);
            }
            
            switch (tray->session_finished_action) {
                case SESSION_FINISHED_SLEEP: {
                    DEBUG_LOG("tray_duration_timer: session_finished_action=SLEEP, starting delayed sleep thread");

                    // Serialize action startup against NoSleep creation failure cleanup.
                    HANDLE sleep_timer = NULL;
                    bool core_init_failed;
                    bool core_init_succeeded;
                    bool stale_session;
                    bool session_starting;
                    AcquireSRWLockExclusive(&tray->delayed_action_lock);
                    stale_session = (tray->timer_thread_id != timer_thread_id);
                    core_init_failed = ATOMIC_LOAD_BOOL(&tray->core_init_failed);
                    core_init_succeeded = ATOMIC_LOAD_BOOL(&tray->core_init_succeeded);
                    session_starting = tray->starting_nosleep;
                    if (!stale_session && !session_starting && !core_init_failed &&
                        core_init_succeeded) {
                        ResetEvent(tray->sleep_stop_event);
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
                        // Show notification about delayed sleep
                        char sleep_message[512];
                        sprintf(sleep_message, "%s\nSystem will sleep in 60 seconds...", duration_message);
                        tray_show_notification(tray, NOTIFY_EVENT_TIMER_EXPIRED,
                            "Time's up!", sleep_message, true);
                        DEBUG_LOG("tray_duration_timer: delayed sleep thread created successfully");
                        // Countdown display will be started by delayed_sleep_thread
                    }
                    break;
                }

                case SESSION_FINISHED_SHUTDOWN: {
                    DEBUG_LOG("tray_duration_timer: session_finished_action=SHUTDOWN, starting delayed shutdown thread");

                    // Serialize action startup against NoSleep creation failure cleanup.
                    HANDLE shutdown_timer = NULL;
                    bool shutdown_core_init_failed;
                    bool shutdown_core_init_succeeded;
                    bool shutdown_stale_session;
                    bool shutdown_session_starting;
                    AcquireSRWLockExclusive(&tray->delayed_action_lock);
                    shutdown_stale_session = (tray->timer_thread_id != timer_thread_id);
                    shutdown_core_init_failed = ATOMIC_LOAD_BOOL(&tray->core_init_failed);
                    shutdown_core_init_succeeded = ATOMIC_LOAD_BOOL(&tray->core_init_succeeded);
                    shutdown_session_starting = tray->starting_nosleep;
                    if (!shutdown_stale_session && !shutdown_session_starting &&
                        !shutdown_core_init_failed && shutdown_core_init_succeeded) {
                        ResetEvent(tray->shutdown_stop_event);
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
                        // Show notification about delayed shutdown
                        char shutdown_message[512];
                        sprintf(shutdown_message, "%s\nSystem will shut down in 60 seconds...", duration_message);
                        tray_show_notification(tray, NOTIFY_EVENT_TIMER_EXPIRED,
                            "Time's up!", shutdown_message, true);
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
    
    // Run nosleep
    int result = nosleep_run(
        ns,
        0, // duration_minutes (0 = indefinite, controlled by timer thread)
        tray->refresh_interval_seconds, // interval_seconds
        tray->prevent_display, // prevent_display
        tray->away_mode, // away_mode
        tray->verbose, // verbose
        tray->stop_event // external stop event
    );
    
    nosleep_destroy(ns);
    
    // If nosleep completed (duration reached or error), update tray state
    DEBUG_LOG("tray_nosleep_thread: nosleep completed, tray->is_running=%s", ATOMIC_LOAD_BOOL(&tray->is_running) ? "true" : "false");
    if (ATOMIC_LOAD_BOOL(&tray->is_running)) {
        DEBUG_LOG("tray_nosleep_thread: calling tray_stop_nosleep with timer_expired=false");
        (void)tray_stop_nosleep_for_session(
            tray, GetCurrentThreadId(), false, false
        );
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
    
    // First method failed, try alternative method
    DWORD error = GetLastError();
    DEBUG_LOG("trigger_system_sleep: SetSuspendState failed with error %lu", error);
    DEBUG_LOG("trigger_system_sleep: trying alternative method via rundll32");
    
    // Try alternative method using rundll32
    STARTUPINFO si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    
    // Create command: rundll32.exe powrprof.dll,SetSuspendState 0,0,0
    char cmd[] = "rundll32.exe powrprof.dll,SetSuspendState 0,0,0";
    
    if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        DEBUG_LOG("trigger_system_sleep: rundll32 process created (PID: %lu)", pi.dwProcessId);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        DWORD alt_error = GetLastError();
        DEBUG_LOG("trigger_system_sleep: rundll32 also failed with error %lu", alt_error);
        DEBUG_LOG("trigger_system_sleep: both sleep methods failed");
        
        // Both methods failed, show notification to user
        tray_show_notification(tray, NOTIFY_EVENT_ACTION_FAILED,
            "Sleep Failed", "Failed to put system to sleep. Check power settings.", true);
    }
}

static void trigger_system_shutdown(NoSleepTray* tray) {
    DEBUG_LOG("trigger_system_shutdown: shutting down system");

    // Reset execution state to allow Windows to shutdown normally
    SetThreadExecutionState(ES_CONTINUOUS);
    DEBUG_LOG("trigger_system_shutdown: execution state reset");

    // First try ExitWindowsEx with EWX_SHUTDOWN | EWX_FORCE
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
    BOOL result = ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCE, 0);
    
    if (result) {
        DEBUG_LOG("trigger_system_shutdown: successfully initiated shutdown");
        return;
    }
    
    // First method failed, try alternative method using InitiateSystemShutdown
    DWORD error = GetLastError();
    DEBUG_LOG("trigger_system_shutdown: ExitWindowsEx failed with error %lu", error);
    DEBUG_LOG("trigger_system_shutdown: trying alternative method via InitiateSystemShutdown");
    
    // Try InitiateSystemShutdown
    result = InitiateSystemShutdown(NULL, NULL, 0, TRUE, TRUE);
    
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

static DWORD WINAPI delayed_sleep_thread(LPVOID lpParam) {
    NoSleepTray* tray = (NoSleepTray*)lpParam;

    DEBUG_LOG("delayed_sleep_thread: waiting 60 seconds before sleep");
    
    // Start countdown display
    tray_start_countdown(tray, SESSION_FINISHED_SLEEP);

    ULONGLONG start_tick64 = GetTickCount64();
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
            } else {
                // Stop and action initiation are serialized by delayed_action_lock.
                DEBUG_LOG("delayed_sleep_thread: 60 seconds elapsed, triggering sleep");
                trigger_system_sleep(tray);
                ReleaseSRWLockExclusive(&tray->delayed_action_lock);
                return 0;
            }
            ReleaseSRWLockExclusive(&tray->delayed_action_lock);
            break;
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

    return 0;
}

static DWORD WINAPI delayed_shutdown_thread(LPVOID lpParam) {
    NoSleepTray* tray = (NoSleepTray*)lpParam;

    DEBUG_LOG("delayed_shutdown_thread: waiting 60 seconds before shutdown");
    
    // Start countdown display
    tray_start_countdown(tray, SESSION_FINISHED_SHUTDOWN);

    ULONGLONG start_tick64 = GetTickCount64();
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
            } else {
                // Stop and action initiation are serialized by delayed_action_lock.
                DEBUG_LOG("delayed_shutdown_thread: 60 seconds elapsed, triggering shutdown");
                trigger_system_shutdown(tray);
                ReleaseSRWLockExclusive(&tray->delayed_action_lock);
                return 0;
            }
            ReleaseSRWLockExclusive(&tray->delayed_action_lock);
            break;
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

    return 0;
}

void tray_start_countdown(NoSleepTray* tray, SessionFinishedAction action) {
    DEBUG_LOG("tray_start_countdown: starting 60-second countdown");
    
    // Stop any existing countdown
    tray_stop_countdown(tray);
    
    // Initialize countdown state
    tray->countdown_action = action;
    ATOMIC_STORE_BOOL(&tray->delayed_sleep_countdown_active, true);
    ATOMIC_STORE_INT(&tray->countdown_seconds, 60);
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
    
    // Prevent re-entrant calls
    if (ATOMIC_LOAD_BOOL(&tray->countdown_stopping)) {
        DEBUG_LOG("tray_stop_countdown: already stopping, returning");
        return;
    }
    
    if (!tray_countdown_has_work(ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active),
                                 tray->countdown_timer_thread != NULL)) {
        DEBUG_LOG("tray_stop_countdown: no active countdown or countdown thread, returning");
        return;
    }
    
    // Mark that we're stopping
    ATOMIC_STORE_BOOL(&tray->countdown_stopping, true);
    
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
    
    ULONGLONG start_tick64 = GetTickCount64();
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
                                      tray->countdown_action == SESSION_FINISHED_SHUTDOWN,
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
        if (tray->session_finished_action == SESSION_FINISHED_SHUTDOWN) {
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

static void tray_update_session_finished_menu(NoSleepTray* tray) {
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
    CheckMenuItem(hSubMenu, IDM_SESSION_FINISHED_SLEEP, 
                  MF_BYCOMMAND | (tray->session_finished_action == SESSION_FINISHED_SLEEP ? MF_CHECKED : MF_UNCHECKED));
    
    // Update main menu item text to show current selection
    char finished_text[64];
    switch (tray->session_finished_action) {
        case SESSION_FINISHED_NONE:
            snprintf(finished_text, sizeof(finished_text), "When finished(None)");
            break;
        case SESSION_FINISHED_SHUTDOWN:
            snprintf(finished_text, sizeof(finished_text), "When finished(Shutdown)");
            break;
        case SESSION_FINISHED_SLEEP:
            snprintf(finished_text, sizeof(finished_text), "When finished(Sleep)");
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

static char* get_exe_path(void) {
    char* path = (char*)malloc(MAX_PATH);
    if (!path) return NULL;
    DWORD len = GetModuleFileName(NULL, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        free(path);
        return NULL;
    }
    return path;
}

// Case-insensitive string comparison (C99-compatible, no strnicmp/strncasecmp)
static int str_icmp_n(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] == '\0' && b[i] == '\0') return 0;
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

// Get the directory containing the executable
static char* get_exe_dir(void) {
    char* path = get_exe_path();
    if (!path) return NULL;
    
    // Find last backslash and terminate there to get directory
    char* last_backslash = strrchr(path, '\\');
    if (last_backslash) {
        *last_backslash = '\0';
    }
    return path; // Caller must free()
}

// Add nosleep directory to user PATH in registry
static bool add_app_to_path(void) {
    char* dir = get_exe_dir();
    if (!dir) return false;
    
    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER,
        "Environment", 0, KEY_READ | KEY_WRITE, &hKey);
    if (result == ERROR_FILE_NOT_FOUND) {
        result = RegCreateKeyEx(HKEY_CURRENT_USER,
            "Environment", 0, NULL, REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_WRITE, NULL, &hKey, NULL);
    }
    if (result != ERROR_SUCCESS) {
        free(dir);
        return false;
    }
    
    // Read current PATH
    DWORD path_size = 0;
    result = RegQueryValueEx(hKey, "Path", NULL, NULL, NULL, &path_size);
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
        RegCloseKey(hKey);
        free(dir);
        return false;
    }
    
    if (path_size == 0) {
        // PATH is empty or doesn't exist, just set it to our directory
        result = RegSetValueEx(hKey, "Path", 0, REG_EXPAND_SZ,
            (LPBYTE)dir, (DWORD)(strlen(dir) + 1));
        RegCloseKey(hKey);
        free(dir);
        
        if (result != ERROR_SUCCESS) return false;
        
        // Notify the system about the environment change
        SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
            (LPARAM)"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
        return true;
    }
    
    // Allocate buffer for current path + new dir + semicolon
    DWORD buf_size = path_size + (DWORD)strlen(dir) + 2; // +2 for ';' and '\0'
    char* new_path = (char*)malloc(buf_size);
    if (!new_path) {
        RegCloseKey(hKey);
        free(dir);
        return false;
    }
    new_path[0] = '\0';
    
    // Read PATH data with retry for TOCTOU safety (PATH may have changed between calls)
    DWORD actual_size = path_size;
    result = RegQueryValueEx(hKey, "Path", NULL, NULL, (LPBYTE)new_path, &actual_size);
    if (result == ERROR_MORE_DATA) {
        // PATH grew between calls, reallocate and retry
        buf_size = actual_size + (DWORD)strlen(dir) + 2;
        char* realloc_path = (char*)realloc(new_path, buf_size);
        if (!realloc_path) {
            RegCloseKey(hKey);
            free(new_path);
            free(dir);
            return false;
        }
        new_path = realloc_path;
        result = RegQueryValueEx(hKey, "Path", NULL, NULL, (LPBYTE)new_path, &actual_size);
    }
    if (result != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        free(new_path);
        free(dir);
        return false;
    }
    // Ensure null-terminated (actual_size is bytes including null in REG_EXPAND_SZ)
    if (actual_size > 0 && actual_size <= buf_size) {
        new_path[actual_size - 1] = '\0'; // REG_EXPAND_SZ includes null terminator in count
    } else {
        new_path[0] = '\0';
    }
        
        // Check if dir is already in PATH (case-insensitive)
        // Remove trailing spaces and semicolons for clean comparison
        size_t dir_len = strlen(dir);
        char* p = new_path;
        bool already_in_path = false;
        while (*p && !already_in_path) {
            // Skip leading spaces
            while (*p == ' ') p++;
            if (*p == '\0') break;
            
            // Find next semicolon or end
            char* next = strchr(p, ';');
            size_t seg_len = next ? (size_t)(next - p) : strlen(p);
            
            // Trim trailing spaces
            while (seg_len > 0 && p[seg_len - 1] == ' ') seg_len--;
            
            if (seg_len == dir_len && str_icmp_n(p, dir, dir_len) == 0) {
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
        size_t existing_len = strlen(new_path);
        if (existing_len > 0 && new_path[existing_len - 1] != ';') {
            strcat(new_path, ";");
        }
    
    // Append new directory
    strcat(new_path, dir);
    
    // Write back to registry
    result = RegSetValueEx(hKey, "Path", 0, REG_EXPAND_SZ,
        (LPBYTE)new_path, (DWORD)(strlen(new_path) + 1));
    
    RegCloseKey(hKey);
    free(new_path);
    free(dir);
    
    if (result != ERROR_SUCCESS) return false;
    
    // Notify the system about the environment change
    SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
        (LPARAM)"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
    
    return true;
}

// Remove nosleep directory from user PATH in registry
static bool remove_app_from_path(void) {
    char* dir = get_exe_dir();
    if (!dir) return false;
    
    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER,
        "Environment", 0, KEY_READ | KEY_WRITE, &hKey);
    if (result != ERROR_SUCCESS) {
        free(dir);
        return result == ERROR_FILE_NOT_FOUND;
    }
    
    // Read current PATH
    DWORD path_size = 0;
    result = RegQueryValueEx(hKey, "Path", NULL, NULL, NULL, &path_size);
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
    
    char* current_path = (char*)malloc(path_size + 1);
    if (!current_path) {
        RegCloseKey(hKey);
        free(dir);
        return false;
    }
    
    // Read PATH data with retry for TOCTOU safety (PATH may have changed between calls)
    DWORD actual_size = path_size;
    DWORD buf_size = path_size;
    result = RegQueryValueEx(hKey, "Path", NULL, NULL, (LPBYTE)current_path, &actual_size);
    if (result == ERROR_MORE_DATA) {
        // PATH grew between calls, reallocate and retry
        buf_size = actual_size;
        char* realloc_path = (char*)realloc(current_path, buf_size + 1);
        if (!realloc_path) {
            free(current_path);
            RegCloseKey(hKey);
            free(dir);
            return false;
        }
        current_path = realloc_path;
        result = RegQueryValueEx(hKey, "Path", NULL, NULL, (LPBYTE)current_path, &actual_size);
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
    // Ensure null-terminated (REG_EXPAND_SZ includes null terminator in count)
    if (actual_size > 0 && actual_size <= buf_size) {
        current_path[actual_size - 1] = '\0';
    } else {
        current_path[0] = '\0';
    }
    
    size_t dir_len = strlen(dir);
    bool found = false;
    
    // Build new path by removing all occurrences of dir (handle duplicates)
    // Use strlen(current_path) since the actual data might be larger than path_size from first query
    size_t new_size = strlen(current_path) + 1;
    char* new_path = (char*)malloc(new_size);
    if (!new_path) {
        free(current_path);
        RegCloseKey(hKey);
        free(dir);
        return false;
    }
    new_path[0] = '\0';
    
    char* p = current_path;
    while (*p) {
        // Find next semicolon or end (without modifying p yet)
        char* seg_start = p;
        char* next = strchr(p, ';');
        size_t seg_len = next ? (size_t)(next - p) : strlen(p);
        
        // Trim leading spaces for comparison only
        char* compare_start = p;
        while (*compare_start == ' ' && compare_start < p + seg_len) compare_start++;
        size_t remaining = seg_len - (size_t)(compare_start - p);
        
        // Trim trailing spaces for comparison
        size_t trimmed_len = remaining;
        while (trimmed_len > 0 && compare_start[trimmed_len - 1] == ' ') trimmed_len--;
        
        // Check if this segment matches our directory
        bool is_match = (trimmed_len == dir_len && str_icmp_n(compare_start, dir, dir_len) == 0);
        
        if (!is_match) {
            // Keep this segment with original formatting
            if (new_path[0] != '\0') {
                strcat(new_path, ";");
            }
            strncat(new_path, seg_start, seg_len);
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
    result = RegSetValueEx(hKey, "Path", 0, REG_EXPAND_SZ,
        (LPBYTE)new_path, (DWORD)(strlen(new_path) + 1));
    
    RegCloseKey(hKey);
    free(new_path);
    free(current_path);
    free(dir);
    
    if (result != ERROR_SUCCESS) return false;
    
    // Notify the system about the environment change
    SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
        (LPARAM)"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
    
    return true;
}

static bool apply_path_preference(bool add_to_path) {
    return add_to_path ? add_app_to_path() : remove_app_from_path();
}

// Check if nosleep directory is currently in the user PATH
static bool is_startup_enabled(void) {
    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) return false;

    char* exe_path = get_exe_path();
    if (!exe_path) {
        RegCloseKey(hKey);
        return false;
    }

    // Build expected registry value: "exe_path" --startup
    // Need to account for possible quotes around exe path
    size_t exe_len = strlen(exe_path);
    size_t expected_len = exe_len + 13;
    char* expected = (char*)malloc(expected_len);
    if (!expected) {
        free(exe_path);
        RegCloseKey(hKey);
        return false;
    }
    snprintf(expected, expected_len, "\"%s\" --startup", exe_path);

    char reg_value[MAX_PATH + 16] = {0};
    DWORD value_size = sizeof(reg_value);
    result = RegQueryValueEx(hKey, "nosleep", NULL, NULL,
                             (LPBYTE)reg_value, &value_size);
    RegCloseKey(hKey);

    bool enabled = (result == ERROR_SUCCESS && strcmp(reg_value, expected) == 0);
    free(exe_path);
    free(expected);
    return enabled;
}

static bool set_startup_registry(bool enable) {
    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_WRITE, &hKey);
    if (enable && result == ERROR_FILE_NOT_FOUND) {
        result = RegCreateKeyEx(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    }
    if (result != ERROR_SUCCESS) {
        return !enable && result == ERROR_FILE_NOT_FOUND;
    }

    if (enable) {
        char* exe_path = get_exe_path();
        if (!exe_path) {
            RegCloseKey(hKey);
            return false;
        }

        // Store as: "exe_path" --startup
        size_t value_len = strlen(exe_path) + 13;
        char* value = (char*)malloc(value_len);
        if (!value) {
            free(exe_path);
            RegCloseKey(hKey);
            return false;
        }
        snprintf(value, value_len, "\"%s\" --startup", exe_path);
        result = RegSetValueEx(hKey, "nosleep", 0, REG_SZ,
                               (LPBYTE)value, (DWORD)(strlen(value) + 1));
        free(value);
        free(exe_path);
    } else {
        result = RegDeleteValue(hKey, "nosleep");
    }

    RegCloseKey(hKey);
    return result == ERROR_SUCCESS || (!enable && result == ERROR_FILE_NOT_FOUND);
}

#define SETTINGS_REG_KEY "Software\\nosleep\\settings"

static void settings_read_dword(HKEY hKey, const char* name, DWORD* value, DWORD default_value) {
    DWORD size = sizeof(DWORD);
    DWORD data = 0;
    LONG result = RegQueryValueEx(hKey, name, NULL, NULL, (LPBYTE)&data, &size);
    if (result == ERROR_SUCCESS) {
        *value = data;
    } else {
        *value = default_value;
    }
}

void tray_load_settings(NoSleepTray* tray) {
    if (!tray) return;
    HKEY hKey;
    LONG result = RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY,
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ, NULL, &hKey, NULL);
    if (result != ERROR_SUCCESS) return;

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
    tray->session_finished_action = (SessionFinishedAction)val;

    RegCloseKey(hKey);
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
        session_finished_action != SESSION_FINISHED_SLEEP) {
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

    HKEY hKey;
    LONG result = RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY,
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (result != ERROR_SUCCESS) return false;

    bool success = true;
    if (session_finished_action >= 0) {
        DWORD val = (DWORD)session_finished_action;
        if (RegSetValueEx(hKey, "session_finished_action", 0, REG_DWORD,
                          (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
            success = false;
        }
    }
    if (auto_start >= 0 && !set_startup_registry(auto_start != 0)) {
        success = false;
    }
    if (notification_mode >= 0) {
        DWORD val = (DWORD)notification_mode;
        if (RegSetValueEx(hKey, "notification_mode", 0, REG_DWORD,
                          (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
            success = false;
        }
    }
    if (auto_check_interval >= 0) {
        DWORD val = (DWORD)auto_check_interval;
        if (RegSetValueEx(hKey, "auto_check_interval", 0, REG_DWORD,
                          (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
            success = false;
        }
    }
    if (check_updates_startup >= 0) {
        DWORD val = (DWORD)(check_updates_startup != 0 ? 1 : 0);
        if (RegSetValueEx(hKey, "check_updates_on_startup", 0, REG_DWORD,
                          (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
            success = false;
        }
    }
    if (add_to_path >= 0) {
        DWORD val = (DWORD)(add_to_path != 0 ? 1 : 0);
        if (RegSetValueEx(hKey, "add_to_path", 0, REG_DWORD,
                          (LPBYTE)&val, sizeof(val)) != ERROR_SUCCESS) {
            success = false;
        }
        if (!apply_path_preference(add_to_path != 0)) {
            success = false;
        }
    }

    RegCloseKey(hKey);
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
    
    // Use balloon notification
    tray->nid.uFlags |= NIF_INFO;
    strcpy(tray->nid.szInfoTitle, full_title);
    strcpy(tray->nid.szInfo, message);
    tray->nid.dwInfoFlags = NIIF_INFO;
    tray->nid.uTimeout = 3000; // 3 seconds
    
    Shell_NotifyIcon(NIM_MODIFY, &tray->nid);
    
    // Reset flags
    tray->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
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
                if (pResult) {
                    *pResult = -1;
                }
                
                // Center dialog on screen
                RECT rc;
                GetWindowRect(hwnd, &rc);
                int screenWidth = GetSystemMetrics(SM_CXSCREEN);
                int screenHeight = GetSystemMetrics(SM_CYSCREEN);
                int x = (screenWidth - (rc.right - rc.left)) / 2;
                int y = (screenHeight - (rc.bottom - rc.top)) / 2;
                SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                
                // Create edit control
                hEdit = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "30",
                    WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_TABSTOP,
                    20, 30, 150, 25,
                    hwnd, NULL, GetModuleHandle(NULL), NULL);
                
                if (hEdit) {
                    // Set font with ClearType quality for high-resolution rendering
                    hInputFont = create_dialog_font(14);
                    if (hInputFont) {
                        SendMessage(hEdit, WM_SETFONT, (WPARAM)hInputFont, TRUE);
                    }
                    
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
            
        case WM_DESTROY:
            if (hInputFont) {
                DeleteObject(hInputFont);
                hInputFont = NULL;
            }
            PostQuitMessage(0);
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
    int result = -1;
    
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
    
    // Create dialog window
    HWND hwndDlg = CreateWindowEx(
        0,
        "NoSleepInputDialog",
        "Custom Duration - nosleep",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT,
        220, 150,
        tray->hwnd,
        NULL,
        hInstance,
        (LPVOID)&result
    );
    
    if (!hwndDlg) {
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
        hwndDlg, NULL, hInstance, NULL);
    
    // Apply high-quality ClearType font to all dialog controls
    HFONT dlgFont = create_dialog_font(14);
    if (dlgFont) {
        EnumChildWindows(hwndDlg, set_child_font_proc, (LPARAM)dlgFont);
        // Store font handle on window for cleanup in WM_DESTROY
        SetWindowLongPtr(hwndDlg, GWLP_USERDATA, (LONG_PTR)dlgFont);
    }
    
    // Show dialog
    ShowWindow(hwndDlg, SW_SHOW);
    
    // Message loop for modal dialog
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(hwndDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    
    // Cleanup
    if (dlgFont) {
        DeleteObject(dlgFont);
    }
    UnregisterClass("NoSleepInputDialog", hInstance);
    
    return result;
}

// Settings dialog control IDs
#define IDC_PREVENT_DISPLAY      2001
#define IDC_AWAY_MODE            2002
#define IDC_AUTO_START           2003
#define IDC_VERBOSE_LOGGING      2004
#define IDC_CHECK_UPDATES_STARTUP 2005
#define IDC_AUTO_CHECK_INTERVAL  2006
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

// Tab indices
#define TAB_GENERAL       0
#define TAB_NOTIFICATIONS 1

// Forward declarations
static LRESULT CALLBACK notify_group_edit_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void create_general_tab(HWND hwnd_tab, NoSleepTray* tray);
static void create_notifications_tab(HWND hwnd_tab, NoSleepTray* tray);
static void refresh_notification_group_list(HWND hwnd_tab, NoSleepTray* tray);

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
                WS_CHILD | WS_VISIBLE | TCS_FIXEDWIDTH,
                10, 10, 460, 350,
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
            hGeneralTab = CreateWindowEx(0, "STATIC", NULL,
                WS_CHILD | WS_VISIBLE,
                15, 35, 450, 320,
                hwnd, NULL, hInst, NULL);
            create_general_tab(hGeneralTab, settings_tray);
            
            // Notifications tab (initially hidden)
            hNotifyTab = CreateWindowEx(0, "STATIC", NULL,
                WS_CHILD,  // Not visible initially
                15, 35, 450, 320,
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
                150, 370, 80, 28, hwnd, (HMENU)IDC_SETTINGS_OK, hInst, NULL);

            CreateWindowEx(0, "BUTTON", "Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                250, 370, 80, 28, hwnd, (HMENU)IDC_SETTINGS_CANCEL, hInst, NULL);

            // Apply font
            HFONT settingsFont = create_dialog_font(14);
            if (settingsFont) {
                EnumChildWindows(hwnd, set_child_font_proc, (LPARAM)settingsFont);
                SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)settingsFont);
            }

            // Center on screen
            RECT rc;
            GetWindowRect(hwnd, &rc);
            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);
            int x = (screenWidth - (rc.right - rc.left)) / 2;
            int yy = (screenHeight - (rc.bottom - rc.top)) / 2;
            SetWindowPos(hwnd, NULL, x, yy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

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
                case IDC_SETTINGS_OK:
                {
                    if (!settings_tray) break;
                    bool path_change_failed = false;
                    
                    // Read general tab settings (controls are children of hGeneralTab)
                    settings_tray->prevent_display = (SendDlgItemMessage(hGeneralTab, IDC_PREVENT_DISPLAY, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    settings_tray->away_mode = (SendDlgItemMessage(hGeneralTab, IDC_AWAY_MODE, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    bool auto_start = (SendDlgItemMessage(hGeneralTab, IDC_AUTO_START, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    if (auto_start != settings_tray->start_on_startup) {
                        tray_set_startup_enabled(settings_tray, auto_start);
                    }
                    settings_tray->verbose = (SendDlgItemMessage(hGeneralTab, IDC_VERBOSE_LOGGING, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    settings_tray->check_updates_on_startup = (SendDlgItemMessage(hGeneralTab, IDC_CHECK_UPDATES_STARTUP, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    if (hIntervalCombo) {
                        int sel = (int)SendMessage(hIntervalCombo, CB_GETCURSEL, 0, 0);
                        tray_apply_auto_check_interval(settings_tray, sel);
                    }
                    bool new_add_to_path = (SendDlgItemMessage(hGeneralTab, IDC_ADD_TO_PATH, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    if (new_add_to_path != settings_tray->add_to_path) {
                        path_change_failed = !tray_set_add_to_path(settings_tray, new_add_to_path);
                    }

                    if (!tray_save_settings_with_warning(hwnd, settings_tray)) {
                        break;
                    }
                    if (path_change_failed) {
                        MessageBox(hwnd,
                            "The PATH change failed. NoSleep will retry applying this setting the next time it starts.",
                            "nosleep - PATH update failed", MB_OK | MB_ICONWARNING);
                    }
                    DestroyWindow(hwnd);
                    break;
                }

                case IDC_SETTINGS_CANCEL:
                    DestroyWindow(hwnd);
                    break;

                case IDC_NOTIFY_ADD_GROUP:
                {
                    show_notify_group_edit_dialog(hwnd, &settings_tray->notify_groups, -1);
                    refresh_notification_group_list(hNotifyTab, settings_tray);
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
            PostQuitMessage(0);
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
    y += 30;

    CreateWindowEx(0, "BUTTON", "Enable away mode",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        20, y, 240, 25, hwnd_parent, (HMENU)IDC_AWAY_MODE, hInst, NULL);
    if (tray->away_mode) {
        SendDlgItemMessage(hwnd_parent, IDC_AWAY_MODE, BM_SETCHECK, BST_CHECKED, 0);
    }
    y += 30;

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
    (void)tray;

    // Create group listbox
    CreateWindowEx(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
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

    // Help text for notification groups
    CreateWindowEx(0, "STATIC", 
        "Notification groups let you control which\n"
        "notification types produce balloon messages.\n"
        "Select a group from the list and click\n"
        "\"Configure...\" to customize which events\n"
        "produce notifications for that group.",
        WS_CHILD | WS_VISIBLE,
        15, 220, 410, 80, hwnd_parent, NULL, hInst, NULL);

    refresh_notification_group_list(hwnd_parent, tray);
}

// Refresh the notification group listbox
static void refresh_notification_group_list(HWND hwnd_parent, NoSleepTray* tray) {
    HWND hList = GetDlgItem(hwnd_parent, IDC_NOTIFY_GROUP_LIST);
    if (!hList || !tray) return;

    SendMessage(hList, LB_RESETCONTENT, 0, 0);

    for (int i = 0; i < tray->notify_groups.count; i++) {
        NotifyGroup* g = &tray->notify_groups.groups[i];
        char display[256];
        const char* marker = (i == tray->notify_groups.active_index) ? " [ACTIVE]" : "";
        snprintf(display, sizeof(display), "%s%s%s", 
                 g->name, marker, g->is_default ? " (default)" : "");
        int idx = (int)SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)display);
        SendMessage(hList, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
    }

    // Select first item by default
    if (tray->notify_groups.count > 0) {
        SendMessage(hList, LB_SETCURSEL, 0, 0);
    }
}

void tray_show_settings_dialog(NoSleepTray* tray) {
    if (!tray) return;

    HINSTANCE hInstance = GetModuleHandle(NULL);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = settings_dialog_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "NoSleepSettingsDialog";

    RegisterClass(&wc);

    HWND hwndDlg = CreateWindowEx(
        0, "NoSleepSettingsDialog", "nosleep Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 490, 440,
        tray->hwnd, NULL, hInstance, (LPVOID)tray
    );

    if (hwndDlg) {
        ShowWindow(hwndDlg, SW_SHOW);

        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) {
            if (!IsDialogMessage(hwndDlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    }

    UnregisterClass("NoSleepSettingsDialog", hInstance);
}

// === Notification Group Edit Dialog ===
// Opens a popup to edit a notification group's name and event checkboxes

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
                SetWindowText(hNameEdit, "My Group");
            }
            
            y += 55;

            // Notification events checkboxes
            CreateWindowEx(0, "STATIC", "Show notifications for these events:",
                WS_CHILD | WS_VISIBLE,
                15, y, 300, 20, hwnd, NULL, hInst, NULL);
            y += 22;

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

            // Resize window to fit content
            RECT rc;
            GetWindowRect(hwnd, &rc);
            int new_width = 460;
            SetWindowPos(hwnd, NULL, 0, 0, new_width, dlg_height, SWP_NOMOVE | SWP_NOZORDER);

            // Apply font
            HFONT editFont = create_dialog_font(14);
            if (editFont) {
                EnumChildWindows(hwnd, set_child_font_proc, (LPARAM)editFont);
                SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)editFont);
            }

            // Center on screen
            GetWindowRect(hwnd, &rc);
            int sw = GetSystemMetrics(SM_CXSCREEN);
            int sh = GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(hwnd, NULL, 
                (sw - (rc.right - rc.left)) / 2,
                (sh - (rc.bottom - rc.top)) / 2,
                0, 0, SWP_NOSIZE | SWP_NOZORDER);

            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
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

                    // Read event mask from checkboxes
                    unsigned int mask = 0;
                    for (int i = 0; i < NOTIFY_EVENT_COUNT; i++) {
                        if (SendMessage(hCheckboxes[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
                            mask |= (1 << i);
                        }
                    }

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

                case IDC_NOTIFY_GROUP_EDIT_CANCEL:
                    DestroyWindow(hwnd);
                    break;
            }
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

    HWND hwndDlg = CreateWindowEx(
        0, "NoSleepNotifyGroupEditDialog", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 400,
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

    UnregisterClass("NoSleepNotifyGroupEditDialog", hInstance);
}

void tray_show_about_dialog(NoSleepTray* tray) {
    if (!tray) return;

    HINSTANCE hInstance = GetModuleHandle(NULL);
    int result = 0;

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
        tray->hwnd, NULL, hInstance, (LPVOID)&result
    );

    if (hwndDlg) {
        ShowWindow(hwndDlg, SW_SHOW);

        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) {
            if (!IsDialogMessage(hwndDlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    }

    UnregisterClass("NoSleepAboutDialog", hInstance);

    if (result == 1) {
        tray_check_for_updates(tray, false);
    }
}

// About dialog control IDs
#define IDC_ABOUT_CHECK_UPDATES 3001
#define IDC_ABOUT_OK            3002

static LRESULT CALLBACK about_dialog_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static int* pResult = NULL;

    switch (msg) {
        case WM_CREATE:
        {
            CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
            pResult = (int*)cs->lpCreateParams;
            if (pResult) *pResult = 0;

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

            CreateWindowEx(0, "BUTTON", "Check for Updates...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
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
                    if (pResult) *pResult = 1;
                    DestroyWindow(hwnd);
                    return TRUE;
                case IDC_ABOUT_OK:
                    if (pResult) *pResult = 0;
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
            pResult = NULL;
            PostQuitMessage(0);
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Update checking - uses updater module
void tray_check_for_updates(NoSleepTray* tray, bool silent) {
    if (!tray) return;

    DEBUG_LOG("tray_check_for_updates: checking for updates");

    UpdateInfo info;
    bool check_ok = updater_check(&info, tray->hwnd);

    save_last_update_check_time();

    if (!check_ok) {
        if (!silent) {
            tray_show_notification(tray, NOTIFY_EVENT_UPDATE_CHECK_FAILED,
                "Update Check Failed", "Could not check for updates. Check your internet connection.", false);
        }
        return;
    }

    if (!info.update_available) {
        if (!silent) {
            tray_show_notification(tray, NOTIFY_EVENT_UPDATE_CHECK_COMPLETED,
                "No Updates",
                "You are running the latest version (v" CURRENT_VERSION ")", false);
        }
        return;
    }

    // Compare versions
    if (updater_compare_versions(info.latest_version, CURRENT_VERSION) <= 0) {
        if (!silent) {
            tray_show_notification(tray, NOTIFY_EVENT_UPDATE_CHECK_COMPLETED,
                "No Updates",
                "You are running the latest version (v" CURRENT_VERSION ")", false);
        }
        return;
    }

    // New version available - show notification
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Version %s is available! (You have v" CURRENT_VERSION ")", info.latest_version);
        tray_show_notification(tray, NOTIFY_EVENT_UPDATE_AVAILABLE, "Update Available", msg, false);
    }

    // Ask user if they want to download
    bool want_download = updater_show_prompt_dialog(tray->hwnd, &info);
    if (!want_download) {
        return;
    }

    // Get current executable path
    char* exe_path = get_exe_path();
    if (!exe_path) {
        MessageBox(tray->hwnd, "Could not determine executable path.", 
                   "Update Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return;
    }

    // Download and install
    bool update_started = updater_download_and_install(&info, exe_path, tray->hwnd);
    free(exe_path);

    if (update_started) {
        // Exit the application so the update can complete
        PostMessage(tray->hwnd, WM_CLOSE, 0, 0);
    }
}

static void tray_apply_auto_check_interval(NoSleepTray* tray, int interval) {
    if (!tray || interval == CB_ERR || tray->auto_check_interval == interval) return;

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
            if (tray_stop_has_work(ATOMIC_LOAD_BOOL(&tray->is_running),
                                   ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active),
                                   tray->sleep_timer != NULL,
                                   tray->shutdown_timer != NULL)) {
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
                        DEBUG_LOG("IDM_STOP: is_running=%s, delayed_sleep_countdown_active=%s, sleep_timer=%s, shutdown_timer=%s",
                                ATOMIC_LOAD_BOOL(&tray->is_running) ? "true" : "false",
                                ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active) ? "true" : "false",
                                tray->sleep_timer ? "active" : "inactive",
                                tray->shutdown_timer ? "active" : "inactive");
                        if (tray_stop_has_work(ATOMIC_LOAD_BOOL(&tray->is_running),
                                               ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active),
                                               tray->sleep_timer != NULL,
                                               tray->shutdown_timer != NULL)) {
                            tray_stop_nosleep(tray, false, false); // show notification when manually stopping
                        }
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
                Shell_NotifyIcon(NIM_DELETE, &tray->nid);
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
                        
                        // Stop any active countdown
                        if (ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active)) {
                            tray_stop_countdown(tray);
                        }
                        
                        // Cancel delayed sleep if active
                        if (tray->sleep_timer) {
                            SetEvent(tray->sleep_stop_event);
                            WaitForSingleObject(tray->sleep_timer, 2000);
                            CloseHandle(tray->sleep_timer);
                            tray->sleep_timer = NULL;
                        }
                        
                        // Cancel delayed shutdown if active
                        if (tray->shutdown_timer) {
                            SetEvent(tray->shutdown_stop_event);
                            WaitForSingleObject(tray->shutdown_timer, 2000);
                            CloseHandle(tray->shutdown_timer);
                            tray->shutdown_timer = NULL;
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
                    if (tray_stop_has_work(ATOMIC_LOAD_BOOL(&tray->is_running),
                                           ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active),
                                           tray->sleep_timer != NULL,
                                           tray->shutdown_timer != NULL)) {
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
