// System tray implementation for nosleep
#include "tray.h"
#include "core.h"
#include "constants.h"
#include "resources.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <powrprof.h>

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

// Forward declarations of helper functions
static void tray_create_icons(NoSleepTray* tray);
static void tray_destroy_icons(NoSleepTray* tray);
static void tray_create_menu(NoSleepTray* tray);
static DWORD WINAPI tray_duration_timer(LPVOID lpParam);
static DWORD WINAPI tray_nosleep_thread(LPVOID lpParam);
static int tray_show_custom_dialog(NoSleepTray* tray);
static HICON create_colored_icon(COLORREF bg_color, bool draw_z);
static HICON create_numbered_icon(int number);
static HICON load_icon_from_resource(LPCTSTR resource_name, int width, int height);
static void load_gray_and_color_icons(NoSleepTray* tray);
static DWORD WINAPI delayed_sleep_thread(LPVOID lpParam);
static DWORD WINAPI delayed_shutdown_thread(LPVOID lpParam);
static void trigger_system_sleep(NoSleepTray* tray);
static void trigger_system_shutdown(NoSleepTray* tray);


NoSleepTray* tray_create(void) {
    DEBUG_PRINT("tray_create: allocating tray structure\n");
    NoSleepTray* tray = (NoSleepTray*)malloc(sizeof(NoSleepTray));
    if (!tray) return NULL;
    
    memset(tray, 0, sizeof(NoSleepTray));
    tray->duration_minutes = -1; // Not set
    tray->current_number = -1;   // No numbered icon displayed
    tray->prevent_display = false;
    tray->away_mode = false;
    tray->verbose = false;
    tray->session_finished_action = SESSION_FINISHED_NONE;
    tray->sleep_after_timeout = false;
    tray->countdown_stopping = false;
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
        CloseHandle(tray->sleep_stop_event);
        CloseHandle(tray->stop_event);
        free(tray);
        return NULL;
    }
    
    return tray;
}

void tray_destroy(NoSleepTray* tray) {
    if (!tray) return;
    
    tray_stop_nosleep(tray, false, false);
    
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
        printf("RegisterClass failed\n");
        return false;
    }
    printf("RegisterClass succeeded\n"); fflush(stdout);
    
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
    printf("CreateWindowEx returned %p\n", tray->hwnd); fflush(stdout);
    if (!tray->hwnd) {
        printf("CreateWindowEx failed\n");
        return false;
    }
    
    // Create icons
    tray_create_icons(tray);
    
    // Create menu
    tray_create_menu(tray);
    
    // Register unique tray message
    tray->uTrayMessage = RegisterWindowMessage("nosleep_tray_message");
    if (tray->uTrayMessage == 0) {
        printf("tray_init: RegisterWindowMessage failed, using default TRAY_ICON_MESSAGE_ID\n"); fflush(stdout);
        tray->uTrayMessage = TRAY_ICON_MESSAGE_ID;
    }
    printf("tray_init: Registered tray message ID: %u (0x%X)\n", tray->uTrayMessage, tray->uTrayMessage); fflush(stdout);
    
    // Setup tray icon
    memset(&tray->nid, 0, sizeof(NOTIFYICONDATA));
    tray->nid.cbSize = sizeof(NOTIFYICONDATA);
    tray->nid.hWnd = tray->hwnd;
    tray->nid.uID = TRAY_ICON_MESSAGE_ID;
    tray->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray->nid.uCallbackMessage = tray->uTrayMessage;
    tray->nid.hIcon = tray->hIconDefault;
    strcpy(tray->nid.szTip, "nosleep - System sleep prevention");
    printf("tray_init: NOTIFYICONDATA size=%lu, hWnd=%p, uCallbackMessage=%u\n", tray->nid.cbSize, tray->nid.hWnd, tray->nid.uCallbackMessage); fflush(stdout);
    
    printf("tray_init: Adding tray icon...\n"); fflush(stdout);
    printf("tray_init: hWnd=%p, uID=%u, uCallbackMessage=%u\n", tray->hwnd, tray->nid.uID, tray->nid.uCallbackMessage); fflush(stdout);
    if (!Shell_NotifyIcon(NIM_ADD, &tray->nid)) {
        printf("tray_init: Shell_NotifyIcon failed with error %lu\n", GetLastError()); fflush(stdout);
        DestroyWindow(tray->hwnd);
        return false;
    }
    printf("tray_init: Tray icon added successfully\n"); fflush(stdout);
    printf("tray_init: Icon added with hWnd=%p, uID=%u, uCallbackMessage=%u\n", tray->nid.hWnd, tray->nid.uID, tray->nid.uCallbackMessage); fflush(stdout);
    
    // Set tray icon version to legacy version (NOTIFYICON_VERSION = 3) for mouse messages
    tray->nid.uVersion = 3; // NOTIFYICON_VERSION
    if (!Shell_NotifyIcon(NIM_SETVERSION, &tray->nid)) {
        printf("tray_init: NIM_SETVERSION failed with error %lu\n", GetLastError()); fflush(stdout);
    } else {
        printf("tray_init: Tray icon version set to %u (legacy mouse messages)\n", tray->nid.uVersion); fflush(stdout);
    }
    
    // Show notification balloon
    tray_show_notification(tray, "nosleep started", "System tray icon created");
    
    // Auto-start test duration if NOSLEEP_DEBUG=1 or 2
    {
        const char* debug = getenv("NOSLEEP_DEBUG");
        (void)debug; // Suppress unused variable warning
    }
    
    return true;
}

void tray_run(NoSleepTray* tray) {
    printf("tray_run entered\n"); fflush(stdout);
    if (!tray || !tray->hwnd) return;
    
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        const char* msg_name = "unknown";
        if (msg.message == WM_NULL) msg_name = "WM_NULL";
        else if (msg.message == WM_CREATE) msg_name = "WM_CREATE";
        else if (msg.message == WM_DESTROY) msg_name = "WM_DESTROY";
        else if (msg.message == WM_COMMAND) msg_name = "WM_COMMAND";
        else if (msg.message == WM_TIMER) msg_name = "WM_TIMER";
        else if (msg.message == WM_USER) msg_name = "WM_USER";
        else if (msg.message == TRAY_ICON_MESSAGE_ID) msg_name = "TRAY_ICON_MESSAGE_ID";
        else if (msg.message == tray->uTrayMessage) msg_name = "REGISTERED_TRAY_MSG";
        printf("tray_run: message 0x%04X (%s) hwnd=%p\n", msg.message, msg_name, msg.hwnd); fflush(stdout);
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

static HICON load_icon_from_resource(LPCTSTR resource_name, int width, int height) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    HINSTANCE hInstance = GetModuleHandle(NULL);
    if (!hInstance) {
        return NULL;
    }
    
    // First try to load the icon with the specified size
    HICON hIcon = (HICON)LoadImage(hInstance, resource_name, IMAGE_ICON, width, height, LR_DEFAULTCOLOR);
    if (!hIcon) {
        // Fallback: load with default size
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] load_icon_from_resource: failed to load icon %p size %dx%d, trying default size\n", resource_name, width, height);
            fflush(stderr);
        }
        hIcon = (HICON)LoadImage(hInstance, resource_name, IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR | LR_DEFAULTSIZE);
    }
    
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] load_icon_from_resource: icon %p %s\n", resource_name, hIcon ? "loaded successfully" : "failed to load");
        fflush(stderr);
    }
    
    return hIcon;
}

static HICON icon_to_grayscale(HICON hColorIcon) {
    if (!hColorIcon) return NULL;
    
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] icon_to_grayscale: converting icon to grayscale\n");
    }
    
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
    
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] icon_to_grayscale: conversion %s\n", hGrayIcon ? "succeeded" : "failed");
    }
    
    return hGrayIcon;
}

static void load_gray_and_color_icons(NoSleepTray* tray) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] load_gray_and_color_icons: loading icons from resources\n");
    }
    
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
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] create_numbered_icon(%d)\n", number);
        fflush(stderr);
    }
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
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] create_numbered_icon: CreateDIBSection %s\n", hbmpColor ? "succeeded" : "failed");
        fflush(stderr);
    }
    if (!hbmpColor) {
        DeleteDC(hdcMem);
        DeleteDC(hdcMask);
        ReleaseDC(NULL, hdc);
        return NULL;
    }
    
    // Create 1-bit mask bitmap (monochrome)
    HBITMAP hbmpMask = CreateBitmap(width, height, 1, 1, NULL);
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] create_numbered_icon: CreateBitmap %s\n", hbmpMask ? "succeeded" : "failed");
        fflush(stderr);
    }
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
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] create_numbered_icon: background pixel[0]=0x%08lX\n", (unsigned long)pixels[0]);
        fflush(stderr);
    }
    
    // Fill mask with white (transparent) initially - mask: 1 = transparent, 0 = opaque
    // We'll set mask to white everywhere (fully transparent), then draw text in black (opaque)
    PatBlt(hdcMask, 0, 0, width, height, WHITENESS);
    
    // Prepare text
    char text[16];
    if (number < 0) {
        strcpy(text, "∞");
    } else {
        snprintf(text, sizeof(text), "%d", number);
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
    hFont = CreateFont(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
    if (!hFont) {
        // Fallback to system font
        hFont = CreateFont(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, NULL);
    }
    
    if (hFont) {
        oldFont = (HFONT)SelectObject(hdcMem, hFont);
        
        // Measure text
        GetTextExtentPoint32(hdcMem, text, (int)strlen(text), &textSize);
        
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
            
            hFont = CreateFont(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
            if (!hFont) {
                hFont = CreateFont(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, NULL);
            }
            
            if (hFont) {
                oldFont = (HFONT)SelectObject(hdcMem, hFont);
                // Remeasure with new font
                GetTextExtentPoint32(hdcMem, text, (int)strlen(text), &textSize);
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
    TextOut(hdcMem, textX, textY, text, (int)strlen(text));
    
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
        TextOut(hdcMask, textX, textY, text, (int)strlen(text));
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
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] create_numbered_icon: CreateIconIndirect %s\n", hIcon ? "succeeded" : "failed");
        fflush(stderr);
    }
    
    // Clean up
    DeleteObject(hbmpColor);
    DeleteObject(hbmpMask);
    DeleteDC(hdcMem);
    DeleteDC(hdcMask);
    ReleaseDC(NULL, hdc);
    
    return hIcon;
}

static HICON create_transparent_icon(void) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] create_transparent_icon: creating fully transparent icon\n");
    }
    
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
    
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] create_transparent_icon: CreateIcon %s\n", hIcon ? "succeeded" : "failed");
    }
    
    free(xor_bits);
    free(and_bits);
    
    // Fallback: try alternative method if CreateIcon fails
    if (!hIcon) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] create_transparent_icon: trying alternative method\n");
        }
        
        // Alternative: create a 32x32 icon with alpha=0
        HDC hdc = GetDC(NULL);
        if (!hdc) {
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] create_transparent_icon: GetDC failed, error=%lu\n", GetLastError());
            }
            return NULL;
        }
        
        HDC hdcMem = CreateCompatibleDC(hdc);
        if (!hdcMem) {
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] create_transparent_icon: CreateCompatibleDC failed, error=%lu\n", GetLastError());
            }
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
                if (!hIcon && debug && strcmp(debug, "1") == 0) {
                    fprintf(stderr, "[nosleep] create_transparent_icon: CreateIconIndirect failed, error=%lu\n", GetLastError());
                }
                
                DeleteObject(hbmpMask);
            } else if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] create_transparent_icon: CreateBitmap failed, error=%lu\n", GetLastError());
            }
            
            DeleteObject(hbmpColor);
        } else if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] create_transparent_icon: CreateDIBSection failed, error=%lu\n", GetLastError());
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
    printf("tray_create_icons called\n"); fflush(stdout);
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_create_icons: loading icons\n");
    }
    // Load color icon from resource and create grayscale version
    load_gray_and_color_icons(tray);
    
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_create_icons: default icon %s, active icon %s\n",
                tray->hIconDefault ? "created" : "failed",
                tray->hIconActive ? "created" : "failed");
    }
    
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
    
    // Pre-create all 60 numbered icons (0-59) for better performance
    for (int i = 0; i < 60; i++) {
        tray->hIconNumbered[i] = create_numbered_icon(i);
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_create_icons: numbered icon %d %s\n",
                    i, tray->hIconNumbered[i] ? "created" : "failed");
        }
    }
    
    // Create transparent icon for countdown blink off state
    tray->hIconCountdownBlank = create_transparent_icon();
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_create_icons: transparent icon %s\n",
                tray->hIconCountdownBlank ? "created" : "failed");
    }
}

static void tray_destroy_icons(NoSleepTray* tray) {
    // Destroy custom icons, but not shared system icons
    if (tray->hIconDefault && tray->hIconDefault != system_icon_default) {
        DestroyIcon(tray->hIconDefault);
    }
    tray->hIconDefault = NULL;
    
    if (tray->hIconActive && tray->hIconActive != system_icon_shield) {
        DestroyIcon(tray->hIconActive);
    }
    tray->hIconActive = NULL;
    
    if (tray->hIconCurrentNumbered) {
        // Only destroy if not cached (0-59)
        if (tray->current_number < 0 || tray->current_number >= 60) {
            DestroyIcon(tray->hIconCurrentNumbered);
        }
        tray->hIconCurrentNumbered = NULL;
        tray->current_number = -1;
    }
    
    for (int i = 0; i < 60; i++) {
        if (tray->hIconNumbered[i]) {
            DestroyIcon(tray->hIconNumbered[i]);
            tray->hIconNumbered[i] = NULL;
        }
    }
    
    if (tray->hIconCountdownBlank) {
        DestroyIcon(tray->hIconCountdownBlank);
        tray->hIconCountdownBlank = NULL;
    }
}

static void tray_create_menu(NoSleepTray* tray) {
    tray->hmenu = CreatePopupMenu();
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        printf("[nosleep] tray_create_menu: CreatePopupMenu returned %p\n", tray->hmenu); fflush(stdout);
    }
    if (!tray->hmenu) {
        if (debug && strcmp(debug, "1") == 0) {
            printf("[nosleep] tray_create_menu: Failed to create menu (error %lu)\n", GetLastError()); fflush(stdout);
        }
        return;
    }
    
    // Duration submenu
    HMENU hSubMenu = CreatePopupMenu();
    if (debug && strcmp(debug, "1") == 0) {
        printf("[nosleep] tray_create_menu: CreatePopupMenu submenu returned %p\n", hSubMenu); fflush(stdout);
    }
    if (!hSubMenu) {
        if (debug && strcmp(debug, "1") == 0) {
            printf("[nosleep] tray_create_menu: Failed to create submenu (error %lu)\n", GetLastError()); fflush(stdout);
        }
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
    AppendMenu(tray->hmenu, MF_STRING | MF_POPUP, (UINT_PTR)hSessionFinishedMenu, "When finished");
    AppendMenu(tray->hmenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(tray->hmenu, MF_STRING | MF_POPUP, (UINT_PTR)hSubMenu, "Set Duration");
    AppendMenu(tray->hmenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(tray->hmenu, MF_STRING, IDM_STOP, "Stop");
    AppendMenu(tray->hmenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(tray->hmenu, MF_STRING, IDM_EXIT, "Exit");
    if (debug && strcmp(debug, "1") == 0) {
        printf("[nosleep] tray_create_menu: Menu created successfully\n"); fflush(stdout);
    }
}

void tray_start_nosleep(NoSleepTray* tray, int duration_minutes) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        printf("tray_start_nosleep(%d)\n", duration_minutes); fflush(stdout);
    }
    if (!tray) {
        return;
    }
    
    // If already running, stop it first
    if (ATOMIC_LOAD_BOOL(&tray->is_running)) {
        tray_stop_nosleep(tray, false, true); // suppress notification when switching
    }
    
    tray->duration_minutes = duration_minutes;
    ATOMIC_STORE_BOOL(&tray->is_running, true);
    ATOMIC_STORE_BOOL(&tray->duration_expired, false);
    ATOMIC_STORE_BOOL(&tray->stopping, false);
    GetSystemTime(&tray->start_time);
    ResetEvent(tray->stop_event);
    
    // Update icon
    tray_update_icon(tray);
    
    // Show notification
    if (duration_minutes > 0) {
        char message[256];
        sprintf(message, "Preventing system sleep for %d minutes", duration_minutes);
        tray_show_notification(tray, "Starting", message);
    } else {
        tray_show_notification(tray, "Starting", "Preventing system sleep indefinitely");
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
        tray_show_notification(tray, "Error", "Failed to create nosleep thread");
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
            tray_stop_nosleep(tray, false, true); // suppress notification
            
            char error_msg[256];
            sprintf(error_msg, "Failed to create timer thread. Error code: %lu", GetLastError());
            tray_show_notification(tray, "Error", error_msg);
            return;
        }
    }
    
    // Update stop menu item text
    tray_update_stop_menu_item(tray);
}

void tray_stop_nosleep(NoSleepTray* tray, bool timer_expired, bool suppress_notification) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_stop_nosleep called with timer_expired=%s, suppress_notification=%s\n", timer_expired ? "true" : "false", suppress_notification ? "true" : "false");
    }
    
    // Prevent re-entrant calls
    if (!tray || ATOMIC_LOAD_BOOL(&tray->stopping)) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_stop_nosleep: already stopping or invalid tray, returning\n");
        }
        return;
    }

    if (!ATOMIC_LOAD_BOOL(&tray->is_running) && !ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active)) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_stop_nosleep: neither running nor in countdown, returning\n");
        }
        return;
    }
    
    ATOMIC_STORE_BOOL(&tray->stopping, true);
    ATOMIC_STORE_BOOL(&tray->is_running, false);
    SetEvent(tray->stop_event);
    
    // Wait for threads to finish
    if (tray->timer_thread) {
        DWORD current_thread_id = GetCurrentThreadId();
        if (current_thread_id != tray->timer_thread_id) {
            WaitForSingleObject(tray->timer_thread, 2000);
        }
        CloseHandle(tray->timer_thread);
        tray->timer_thread = NULL;
    }
    
    if (tray->nosleep_thread) {
        DWORD current_thread_id = GetCurrentThreadId();
        if (current_thread_id != tray->nosleep_thread_id) {
            WaitForSingleObject(tray->nosleep_thread, 2000);
        }
        CloseHandle(tray->nosleep_thread);
        tray->nosleep_thread = NULL;
    }
    
    // Cancel and wait for sleep timer thread if active
    if (tray->sleep_timer) {
        // Signal cancellation
        SetEvent(tray->sleep_stop_event);
        
        DWORD current_thread_id = GetCurrentThreadId();
        DWORD sleep_timer_thread_id = GetThreadId(tray->sleep_timer);
        if (current_thread_id != sleep_timer_thread_id) {
            WaitForSingleObject(tray->sleep_timer, 2000);
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
            WaitForSingleObject(tray->shutdown_timer, 2000);
        }
        CloseHandle(tray->shutdown_timer);
        tray->shutdown_timer = NULL;
        
        // Reset the event for future use
        ResetEvent(tray->shutdown_stop_event);
    }
    
    // Cancel sleep if active
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_stop_nosleep: calling tray_stop_countdown, delayed_sleep_countdown_active=%s\n", 
                ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active) ? "true" : "false");
    }
    bool was_countdown_active = ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active);
    int countdown_action = tray->session_finished_action; // SESSION_FINISHED_SLEEP or SESSION_FINISHED_SHUTDOWN
    tray_stop_countdown(tray);

    // Reset execution state to allow Windows to sleep normally
    // This ensures that any previous ES_SYSTEM_REQUIRED flags are cleared
    SetThreadExecutionState(ES_CONTINUOUS);
    
    // Calculate elapsed time
    SYSTEMTIME now;
    GetSystemTime(&now);
    
    FILETIME ft_start, ft_now;
    SystemTimeToFileTime(&tray->start_time, &ft_start);
    SystemTimeToFileTime(&now, &ft_now);
    
    ULARGE_INTEGER uli_start, uli_now;
    uli_start.LowPart = ft_start.dwLowDateTime;
    uli_start.HighPart = ft_start.dwHighDateTime;
    uli_now.LowPart = ft_now.dwLowDateTime;
    uli_now.HighPart = ft_now.dwHighDateTime;
    
    ULONGLONG elapsed_100ns = uli_now.QuadPart - uli_start.QuadPart;
    ULONGLONG elapsed_seconds = elapsed_100ns / 10000000LL;
    
    int hours = (int)(elapsed_seconds / 3600);
    int minutes = (int)((elapsed_seconds % 3600) / 60);
    int seconds = (int)(elapsed_seconds % 60);
    
    char message[256];
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_stop_nosleep: timer_expired=%s, duration_expired=%s, was_countdown_active=%s\n", timer_expired ? "true" : "false", ATOMIC_LOAD_BOOL(&tray->duration_expired) ? "true" : "false", was_countdown_active ? "true" : "false");
    }
    
    // Determine which notification to show
    if (!suppress_notification) {
        if (was_countdown_active) {
            // Countdown was cancelled
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] tray_stop_nosleep: showing countdown cancellation notification\n");
            }
            if (countdown_action == SESSION_FINISHED_SHUTDOWN) {
                tray_show_notification(tray, "Shutdown cancelled", "System shutdown has been cancelled");
            } else {
                tray_show_notification(tray, "Sleep cancelled", "System sleep has been cancelled");
            }
        } else if (ATOMIC_LOAD_BOOL(&tray->duration_expired)) {
            // Timer expired (nosleep session finished naturally)
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] tray_stop_nosleep: showing Time's up! notification\n");
            }
            if (hours > 0) {
                sprintf(message, "Sleep prevention stopped\nDuration: %dh %dm", hours, minutes);
            } else {
                sprintf(message, "Sleep prevention stopped\nDuration: %dm %ds", minutes, seconds);
            }
            tray_show_notification(tray, "Time's up!", message);
        } else {
            // Nosleep session manually stopped
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] tray_stop_nosleep: showing Stopped notification\n");
            }
            if (hours > 0) {
                sprintf(message, "Sleep prevention manually stopped\nTotal duration: %dh %dm", hours, minutes);
            } else {
                sprintf(message, "Sleep prevention manually stopped\nTotal duration: %dm %ds", minutes, seconds);
            }
            tray_show_notification(tray, "Stopped", message);
        }
    } else {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_stop_nosleep: notification suppressed\n");
        }
    }
    
    // Update stop menu item text
    tray_update_stop_menu_item(tray);
    
    // Update icon
    tray_update_icon(tray);
    
    // Reset stopping flag
    ATOMIC_STORE_BOOL(&tray->stopping, false);
}

static DWORD WINAPI tray_duration_timer(LPVOID lpParam) {
    NoSleepTray* tray = (NoSleepTray*)lpParam;
    const char* debug = getenv("NOSLEEP_DEBUG");
    
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_duration_timer: started, duration_minutes=%d, session_finished_action=%d\n",
                tray->duration_minutes, tray->session_finished_action);
    }
    
    if (tray->duration_minutes <= 0) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_duration_timer: duration_minutes=%d, returning early\n", tray->duration_minutes);
        }
        return 0;
    }
    
    DWORD duration_ms = tray->duration_minutes * 60 * 1000;
    DWORD start_tick = GetTickCount();
    
    while (ATOMIC_LOAD_BOOL(&tray->is_running)) {
        DWORD elapsed = GetTickCount() - start_tick;
        if (elapsed >= duration_ms) {
            // Duration reached
            const char* debug = getenv("NOSLEEP_DEBUG");
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] tray_duration_timer: duration reached, stopping with timer_expired=true\n");
            }
            ATOMIC_STORE_BOOL(&tray->duration_expired, true);
            
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] tray_duration_timer: elapsed=%lu ms, duration_ms=%lu, calling tray_stop_nosleep\n",
                        elapsed, duration_ms);
            }
            
            tray_stop_nosleep(tray, true, false); // show notification for timer expiry
            
            // Check what action to take when session finishes
            switch (tray->session_finished_action) {
                case SESSION_FINISHED_SLEEP:
                    if (debug && strcmp(debug, "1") == 0) {
                        fprintf(stderr, "[nosleep] tray_duration_timer: session_finished_action=SLEEP, starting delayed sleep thread\n");
                    }
                    
                    // Reset event before starting new sleep timer
                    ResetEvent(tray->sleep_stop_event);
                    
                    // Create delayed sleep thread
                    tray->sleep_timer = CreateThread(
                        NULL, 0, delayed_sleep_thread, tray, 0, NULL
                    );
                    if (!tray->sleep_timer) {
                        if (debug && strcmp(debug, "1") == 0) {
                            fprintf(stderr, "[nosleep] tray_duration_timer: failed to create sleep timer thread\n");
                        }
                    } else {
                        // Show notification about delayed sleep
                        tray_show_notification(tray, "Time's up!", "System will sleep in 60 seconds...");
                        if (debug && strcmp(debug, "1") == 0) {
                            fprintf(stderr, "[nosleep] tray_duration_timer: delayed sleep thread created successfully\n");
                        }
                        // Countdown display will be started by delayed_sleep_thread
                    }
                    break;
                    
                case SESSION_FINISHED_SHUTDOWN:
                    if (debug && strcmp(debug, "1") == 0) {
                        fprintf(stderr, "[nosleep] tray_duration_timer: session_finished_action=SHUTDOWN, starting delayed shutdown thread\n");
                    }
                    
                    // Reset event before starting new shutdown timer
                    ResetEvent(tray->shutdown_stop_event);
                    
                    // Create delayed shutdown thread
                    tray->shutdown_timer = CreateThread(
                        NULL, 0, delayed_shutdown_thread, tray, 0, NULL
                    );
                    if (!tray->shutdown_timer) {
                        if (debug && strcmp(debug, "1") == 0) {
                            fprintf(stderr, "[nosleep] tray_duration_timer: failed to create shutdown timer thread\n");
                        }
                    } else {
                        // Show notification about delayed shutdown
                        tray_show_notification(tray, "Time's up!", "System will shut down in 60 seconds...");
                        if (debug && strcmp(debug, "1") == 0) {
                            fprintf(stderr, "[nosleep] tray_duration_timer: delayed shutdown thread created successfully\n");
                        }
                        // Countdown display will be started by delayed_shutdown_thread
                    }
                    break;
                    
                case SESSION_FINISHED_NONE:
                default:
                    if (debug && strcmp(debug, "1") == 0) {
                        fprintf(stderr, "[nosleep] tray_duration_timer: session_finished_action=NONE, no action\n");
                    }
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
        tray_show_notification(tray, "Error", "Failed to create NoSleep instance");
        return 1;
    }
    
    // Run nosleep
    int result = nosleep_run(
        ns,
        0, // duration_minutes (0 = indefinite, controlled by timer thread)
        20, // interval_seconds
        tray->prevent_display, // prevent_display
        tray->away_mode, // away_mode
        tray->verbose  // verbose
    );
    
    nosleep_destroy(ns);
    
    // If nosleep completed (duration reached or error), update tray state
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_nosleep_thread: nosleep completed, tray->is_running=%s\n", ATOMIC_LOAD_BOOL(&tray->is_running) ? "true" : "false");
    }
    if (ATOMIC_LOAD_BOOL(&tray->is_running)) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_nosleep_thread: calling tray_stop_nosleep with timer_expired=false\n");
        }
        tray_stop_nosleep(tray, false, false);
    }
    
    return result;
}

static void trigger_system_sleep(NoSleepTray* tray) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] trigger_system_sleep: putting system to sleep\n");
    }

    // First, reset execution state to ensure Windows can sleep
    SetThreadExecutionState(ES_CONTINUOUS);
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] trigger_system_sleep: execution state reset\n");
    }

    // Use SetSuspendState to put system to sleep (not hibernate)
    // First parameter FALSE = sleep (not hibernate)
    // Second parameter FALSE = force sleep (do not ask applications)
    // Third parameter FALSE = disable wake events
    BOOL result = SetSuspendState(FALSE, FALSE, FALSE);

    if (result) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] trigger_system_sleep: successfully initiated sleep\n");
        }
        return;
    }
    
    // First method failed, try alternative method
    DWORD error = GetLastError();
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] trigger_system_sleep: SetSuspendState failed with error %lu\n", error);
        fprintf(stderr, "[nosleep] trigger_system_sleep: trying alternative method via rundll32\n");
    }
    
    // Try alternative method using rundll32
    STARTUPINFO si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    
    // Create command: rundll32.exe powrprof.dll,SetSuspendState 0,0,0
    char cmd[] = "rundll32.exe powrprof.dll,SetSuspendState 0,0,0";
    
    if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] trigger_system_sleep: rundll32 process created (PID: %lu)\n", pi.dwProcessId);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        DWORD alt_error = GetLastError();
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] trigger_system_sleep: rundll32 also failed with error %lu\n", alt_error);
            fprintf(stderr, "[nosleep] trigger_system_sleep: both sleep methods failed\n");
        }
        
        // Both methods failed, show notification to user
        tray_show_notification(tray, "Sleep Failed", "Failed to put system to sleep. Check power settings.");
    }
}

static void trigger_system_shutdown(NoSleepTray* tray) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] trigger_system_shutdown: shutting down system\n");
    }

    // Reset execution state to allow Windows to shutdown normally
    SetThreadExecutionState(ES_CONTINUOUS);
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] trigger_system_shutdown: execution state reset\n");
    }

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
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] trigger_system_shutdown: successfully initiated shutdown\n");
        }
        return;
    }
    
    // First method failed, try alternative method using InitiateSystemShutdown
    DWORD error = GetLastError();
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] trigger_system_shutdown: ExitWindowsEx failed with error %lu\n", error);
        fprintf(stderr, "[nosleep] trigger_system_shutdown: trying alternative method via InitiateSystemShutdown\n");
    }
    
    // Try InitiateSystemShutdown
    result = InitiateSystemShutdown(NULL, NULL, 0, TRUE, TRUE);
    
    if (result) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] trigger_system_shutdown: InitiateSystemShutdown succeeded\n");
        }
        return;
    }
    
    // Both methods failed
    DWORD alt_error = GetLastError();
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] trigger_system_shutdown: InitiateSystemShutdown also failed with error %lu\n", alt_error);
        fprintf(stderr, "[nosleep] trigger_system_shutdown: both shutdown methods failed\n");
    }
    
    // Show notification to user
    tray_show_notification(tray, "Shutdown Failed", "Failed to shut down system. Check permissions.");
}

static DWORD WINAPI delayed_sleep_thread(LPVOID lpParam) {
    NoSleepTray* tray = (NoSleepTray*)lpParam;
    const char* debug = getenv("NOSLEEP_DEBUG");

    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] delayed_sleep_thread: waiting 60 seconds before sleep\n");
    }
    
    // Start countdown display
    tray_start_countdown(tray);

    // Wait for 60 seconds or until cancelled
    DWORD wait_result = WaitForSingleObject(tray->sleep_stop_event, 60000);

    if (wait_result == WAIT_OBJECT_0) {
        // Cancelled
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] delayed_sleep_thread: cancelled, not sleeping\n");
        }
    } else {
        // Timeout reached, but check if we should still sleep
        // Double-check stop condition to prevent race condition
        if (ATOMIC_LOAD_BOOL(&tray->stopping) || WaitForSingleObject(tray->sleep_stop_event, 0) == WAIT_OBJECT_0) {
            // Already stopped, don't trigger sleep
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] delayed_sleep_thread: race condition detected, not sleeping\n");
            }
        } else {
            // Trigger sleep
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] delayed_sleep_thread: 60 seconds elapsed, triggering sleep\n");
            }
            // Stop countdown display before sleep
            tray_stop_countdown(tray);
            trigger_system_sleep(tray);
            return 0;
        }
    }
    
    // Cancel sleep display (only reached if sleep was cancelled)
    tray_stop_countdown(tray);

    return 0;
}

static DWORD WINAPI delayed_shutdown_thread(LPVOID lpParam) {
    NoSleepTray* tray = (NoSleepTray*)lpParam;
    const char* debug = getenv("NOSLEEP_DEBUG");

    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] delayed_shutdown_thread: waiting 60 seconds before shutdown\n");
    }
    
    // Start countdown display
    tray_start_countdown(tray);

    // Wait for 60 seconds or until cancelled
    DWORD wait_result = WaitForSingleObject(tray->shutdown_stop_event, 60000);

    if (wait_result == WAIT_OBJECT_0) {
        // Cancelled
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] delayed_shutdown_thread: cancelled, not shutting down\n");
        }
    } else {
        // Timeout reached, but check if we should still shutdown
        // Double-check stop condition to prevent race condition
        if (ATOMIC_LOAD_BOOL(&tray->stopping) || WaitForSingleObject(tray->shutdown_stop_event, 0) == WAIT_OBJECT_0) {
            // Already stopped, don't trigger shutdown
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] delayed_shutdown_thread: race condition detected, not shutting down\n");
            }
        } else {
            // Trigger shutdown
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] delayed_shutdown_thread: 60 seconds elapsed, triggering shutdown\n");
            }
            trigger_system_shutdown(tray);
        }
    }
    
    // Cancel countdown display
    tray_stop_countdown(tray);

    return 0;
}

void tray_start_countdown(NoSleepTray* tray) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_start_countdown: starting 60-second countdown\n");
    }
    
    // Stop any existing countdown
    tray_stop_countdown(tray);
    
    // Initialize countdown state
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
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_start_countdown: failed to create countdown thread\n");
        }
    } else {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_start_countdown: countdown thread created\n");
        }
    }
    
    // Update icon immediately to show initial countdown state
    tray_update_icon(tray);
    
    // Update stop menu item text
    tray_update_stop_menu_item(tray);
}

void tray_stop_countdown(NoSleepTray* tray) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_stop_countdown: stopping countdown, delayed_sleep_countdown_active=%s, countdown_stopping=%s\n",
                ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active) ? "true" : "false",
                ATOMIC_LOAD_BOOL(&tray->countdown_stopping) ? "true" : "false");
    }
    
    // Prevent re-entrant calls
    if (ATOMIC_LOAD_BOOL(&tray->countdown_stopping)) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_stop_countdown: already stopping, returning\n");
        }
        return;
    }
    
    if (!ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active)) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_stop_countdown: already not active, returning\n");
        }
        return;
    }
    
    // Mark that we're stopping
    ATOMIC_STORE_BOOL(&tray->countdown_stopping, true);
    
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_stop_countdown: setting delayed_sleep_countdown_active=false\n");
    }
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
    const char* debug = getenv("NOSLEEP_DEBUG");
    
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] countdown_thread: started\n");
    }
    
    int tick_count = 0;
    
    while (ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active) && ATOMIC_LOAD_INT(&tray->countdown_seconds) > 0) {
        // Wait for 500ms (half second) or stop event for blinking
        DWORD wait_result = WaitForSingleObject(tray->countdown_stop_event, 500);
        
        if (wait_result == WAIT_OBJECT_0) {
            // Stop event signaled
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] countdown_thread: stop event signaled, delayed_sleep_countdown_active=%s, countdown_seconds=%d\n",
                        ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active) ? "true" : "false", ATOMIC_LOAD_INT(&tray->countdown_seconds));
            }
            break;
        }
        
        // Toggle blink state every 500ms
        bool old_blink_state = ATOMIC_LOAD_BOOL(&tray->countdown_blink_state);
        ATOMIC_STORE_BOOL(&tray->countdown_blink_state, !old_blink_state);
        tick_count++;
        
        // Update seconds every 2 ticks (1000ms = 1 second)
        if (tick_count % 2 == 0) {
            int old_seconds = ATOMIC_LOAD_INT(&tray->countdown_seconds);
            ATOMIC_STORE_INT(&tray->countdown_seconds, old_seconds - 1);
        }
        
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] countdown_thread: seconds=%d, blink=%s, tick=%d\n", 
                    ATOMIC_LOAD_INT(&tray->countdown_seconds), ATOMIC_LOAD_BOOL(&tray->countdown_blink_state) ? "show" : "hide", tick_count);
        }
        
        // Update icon
        tray_update_icon(tray);
        
        // Check if countdown finished
        if (ATOMIC_LOAD_INT(&tray->countdown_seconds) <= 0) {
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] countdown_thread: countdown finished\n");
            }
            break;
        }
    }
    
    ATOMIC_STORE_BOOL(&tray->delayed_sleep_countdown_active, false);
    // Ensure memory visibility across threads
    MEMORY_BARRIER();
    
    // Update icon and stop menu item when countdown finishes naturally
    tray_update_icon(tray);
    tray_update_stop_menu_item(tray);
    
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] countdown_thread: exiting, delayed_sleep_countdown_active=%s\n",
                ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active) ? "true" : "false");
    }
    
    return 0;
}

void tray_update_icon(NoSleepTray* tray) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    int remaining_minutes = -1;
    if (!tray || !tray->hwnd) return;
    
    // Take local snapshots of atomic variables for consistency
    bool delayed_sleep_countdown_active = ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active);
    int countdown_seconds = ATOMIC_LOAD_INT(&tray->countdown_seconds);
    bool countdown_blink_state = ATOMIC_LOAD_BOOL(&tray->countdown_blink_state);
    bool is_running = ATOMIC_LOAD_BOOL(&tray->is_running);
    
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_update_icon: delayed_sleep_countdown_active=%s\n",
                delayed_sleep_countdown_active ? "true" : "false");
    }
    
    // Handle delayed sleep countdown display
    if (delayed_sleep_countdown_active) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_update_icon: countdown active, seconds=%d, blink=%s\n",
                    countdown_seconds, countdown_blink_state ? "show" : "hide");
        }
        
        if (countdown_blink_state) {
            // Show number icon for current countdown seconds
            HICON numbered_icon = NULL;
            if (countdown_seconds >= 0 && countdown_seconds < 60) {
                // Use cached icon if available
                if (tray->hIconNumbered[countdown_seconds] == NULL) {
                    tray->hIconNumbered[countdown_seconds] = create_numbered_icon(countdown_seconds);
                }
                numbered_icon = tray->hIconNumbered[countdown_seconds];
            } else {
                // Create new icon for numbers outside cache range
                numbered_icon = create_numbered_icon(countdown_seconds);
            }
            
            if (numbered_icon) {
                tray->nid.hIcon = numbered_icon;
            } else {
                tray->nid.hIcon = tray->hIconDefault;
            }
        } else {
            // Blink off: show transparent icon (or default if transparent creation failed)
            tray->nid.hIcon = tray->hIconCountdownBlank ? tray->hIconCountdownBlank : tray->hIconDefault;
        }
        
        // Update tooltip with remaining seconds
        char tip[128];
        if (tray->session_finished_action == SESSION_FINISHED_SHUTDOWN) {
            sprintf(tip, "nosleep - System will shut down in %d seconds", countdown_seconds);
        } else {
            sprintf(tip, "nosleep - System will sleep in %d seconds", countdown_seconds);
        }
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
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_update_icon: Shell_NotifyIcon (countdown) %s after %d attempts\n", 
                    success ? "succeeded" : "failed", retry_count);
            fflush(stderr);
        }
        return;
    }
    
    if (is_running) {
        if (tray->duration_minutes > 0) {
            // Calculate remaining minutes
            SYSTEMTIME now;
            GetSystemTime(&now);
            
            FILETIME ft_start, ft_now;
            SystemTimeToFileTime(&tray->start_time, &ft_start);
            SystemTimeToFileTime(&now, &ft_now);
            
            ULARGE_INTEGER uli_start, uli_now;
            uli_start.LowPart = ft_start.dwLowDateTime;
            uli_start.HighPart = ft_start.dwHighDateTime;
            uli_now.LowPart = ft_now.dwLowDateTime;
            uli_now.HighPart = ft_now.dwHighDateTime;
            
            ULONGLONG elapsed_100ns = uli_now.QuadPart - uli_start.QuadPart;
            ULONGLONG elapsed_seconds = elapsed_100ns / 10000000LL;
            ULONGLONG total_seconds = tray->duration_minutes * 60ULL;
            ULONGLONG remaining_seconds = total_seconds > elapsed_seconds ? total_seconds - elapsed_seconds : 0;
            remaining_minutes = (int)(remaining_seconds / 60);
            int display_number = remaining_minutes > 0 ? remaining_minutes : (int)remaining_seconds;
            
            // Update icon with number
            if (display_number != tray->current_number) {
                if (debug && strcmp(debug, "1") == 0) {
                    fprintf(stderr, "[nosleep] tray_update_icon: display_number=%d current_number=%d remaining_minutes=%d remaining_seconds=%llu\n", display_number, tray->current_number, remaining_minutes, remaining_seconds);
                }
                // Destroy previous numbered icon if not cached (0-59)
                if (tray->hIconCurrentNumbered) {
                    // If previous number was cached (0-59), keep it in array
                    if (tray->current_number < 0 || tray->current_number >= 60) {
                        DestroyIcon(tray->hIconCurrentNumbered);
                    }
                    tray->hIconCurrentNumbered = NULL;
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
                    if (debug && strcmp(debug, "1") == 0) {
                        fprintf(stderr, "[nosleep] tray_update_icon: numbered icon creation failed, using active icon\n");
                    }
                    tray->hIconCurrentNumbered = tray->hIconActive;
                }
                tray->current_number = display_number;
            }
            
            tray->nid.hIcon = tray->hIconCurrentNumbered ? tray->hIconCurrentNumbered : tray->hIconActive;
            
            // Update tooltip
            char tip[128];
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] tray_update_icon: remaining_seconds=%llu remaining_minutes=%d\n", remaining_seconds, remaining_minutes);
            }
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
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] tray_update_icon: tooltip='%s'\n", tip);
            }
        } else {
            // Indefinite
            // Clean up any numbered icon currently displayed
            if (tray->hIconCurrentNumbered) {
                // If not cached (-1 or >=60), destroy it
                if (tray->current_number < 0 || tray->current_number >= 60) {
                    DestroyIcon(tray->hIconCurrentNumbered);
                }
                tray->hIconCurrentNumbered = NULL;
                tray->current_number = -1;
            }
            
            // Create or get infinite icon
            if (tray->current_number != -1) {
                // Create infinite icon if needed
                tray->hIconCurrentNumbered = create_numbered_icon(-1);
                tray->current_number = -1;
            }
            
            tray->nid.hIcon = tray->hIconCurrentNumbered ? tray->hIconCurrentNumbered : tray->hIconActive;
            strcpy(tray->nid.szTip, "nosleep - Active (indefinite)");
        }
    } else {
        // Not running
        tray->nid.hIcon = tray->hIconDefault;
        strcpy(tray->nid.szTip, "nosleep - System sleep prevention");
    }
    
    tray->nid.uFlags = NIF_ICON | NIF_TIP;
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_update_icon: setting hIcon=%p, remaining_minutes=%d\n", tray->nid.hIcon, remaining_minutes);
        fflush(stderr);
    }
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
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_update_icon: Shell_NotifyIcon %s after %d attempts\n", success ? "succeeded" : "failed", retry_count);
        fflush(stderr);
    }
}

void tray_update_stop_menu_item(NoSleepTray* tray) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (!tray || !tray->hmenu) return;
    
    // Get current state
    bool delayed_sleep_countdown_active = ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active);
    bool is_running = ATOMIC_LOAD_BOOL(&tray->is_running);
    
    // Determine menu text based on state
    const char* stop_text = NULL;
    if (delayed_sleep_countdown_active) {
        // Check if it's shutdown or sleep countdown
        if (tray->session_finished_action == SESSION_FINISHED_SHUTDOWN) {
            stop_text = "Stop shutdown";
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
    
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_update_stop_menu_item: updated to '%s' (countdown_active=%s, is_running=%s, session_action=%d)\n",
                stop_text, delayed_sleep_countdown_active ? "true" : "false", is_running ? "true" : "false", tray->session_finished_action);
    }
}

static void tray_update_session_finished_menu(NoSleepTray* tray) {
    if (!tray || !tray->hmenu) return;
    
    // Get the When finished submenu (position 0 in main menu)
    HMENU hSubMenu = GetSubMenu(tray->hmenu, 0);
    if (!hSubMenu) return;
    
    // Update checkmarks for all three radio items in the submenu
    CheckMenuItem(hSubMenu, IDM_SESSION_FINISHED_NONE, 
                  MF_BYCOMMAND | (tray->session_finished_action == SESSION_FINISHED_NONE ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_SESSION_FINISHED_SHUTDOWN, 
                  MF_BYCOMMAND | (tray->session_finished_action == SESSION_FINISHED_SHUTDOWN ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_SESSION_FINISHED_SLEEP, 
                  MF_BYCOMMAND | (tray->session_finished_action == SESSION_FINISHED_SLEEP ? MF_CHECKED : MF_UNCHECKED));
}

void tray_show_notification(NoSleepTray* tray, const char* title, const char* message) {
    if (!tray || !tray->hwnd) return;
    
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug) {
        fprintf(stderr, "[nosleep] tray_show_notification: title='%s', message='%s'\n", title, message);
        fflush(stderr);
    }
    
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
                    // Set font
                    HFONT hFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
                    if (hFont) {
                        SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
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
    UnregisterClass("NoSleepInputDialog", hInstance);
    
    return result;
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
        const char* debug = getenv("NOSLEEP_DEBUG");

        if (debug && (strcmp(debug, "1") == 0 || strcmp(debug, "2") == 0)) {
            printf("[nosleep] tray_window_proc: Tray message received (msg=0x%X) wParam=%lld, lParam=%lld (%s)\n", msg, wParam, lParam, msg_name); fflush(stdout);
            // Decode high and low words of lParam for debugging
            printf("[nosleep] tray_window_proc: lParam low word=0x%04X, high word=0x%04X\n", LOWORD(lParam), HIWORD(lParam)); fflush(stdout);
        }
        
        // Handle right-click (both legacy mouse message and notification code)
        if (lParam == WM_RBUTTONUP || lParam == NIN_KEYSELECT) {
            // Show context menu
            if (debug && (strcmp(debug, "1") == 0 || strcmp(debug, "2") == 0)) {
                printf("[nosleep] tray_window_proc: Right-click detected, showing menu\n"); fflush(stdout);
            }
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd); // Required for menu to disappear properly
            // Enable/disable Stop menu item based on running state
            if (ATOMIC_LOAD_BOOL(&tray->is_running) || ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active)) {
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
                        const char* debug = getenv("NOSLEEP_DEBUG");
                        if (debug && strcmp(debug, "1") == 0) {
                            fprintf(stderr, "[nosleep] IDM_STOP: is_running=%s, delayed_sleep_countdown_active=%s\n",
                                    ATOMIC_LOAD_BOOL(&tray->is_running) ? "true" : "false",
                                    ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active) ? "true" : "false");
                        }
                        if (ATOMIC_LOAD_BOOL(&tray->is_running) || ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active)) {
                            tray_stop_nosleep(tray, false, false); // show notification when manually stopping
                        }
                    }
                    break;
                case IDM_EXIT:
                    DestroyWindow(hwnd);
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
                    break;
                case IDM_SESSION_FINISHED_SHUTDOWN:
                    tray->session_finished_action = SESSION_FINISHED_SHUTDOWN;
                    tray->sleep_after_timeout = false;
                    tray_update_session_finished_menu(tray);
                    break;
                case IDM_SESSION_FINISHED_SLEEP:
                    tray->session_finished_action = SESSION_FINISHED_SLEEP;
                    tray->sleep_after_timeout = true;
                    tray_update_session_finished_menu(tray);
                    break;
            }
            break;
            
        case WM_TIMER:
            if (wParam == TIMER_ID_AUTO_START) {
                KillTimer(hwnd, TIMER_ID_AUTO_START);
                printf("Auto-starting 1 minute nosleep (testing)\n"); fflush(stdout);
                tray_start_nosleep(tray, 1); // 1 minute for testing
            }
            break;
            
        case WM_DESTROY:
            // Remove tray icon
            if (tray) {
                Shell_NotifyIcon(NIM_DELETE, &tray->nid);
            }
            PostQuitMessage(0);
            break;
            
        case WM_CONTEXTMENU:
            {
                const char* debug = getenv("NOSLEEP_DEBUG");
                if (debug && (strcmp(debug, "1") == 0 || strcmp(debug, "2") == 0)) {
                    printf("[nosleep] tray_window_proc: WM_CONTEXTMENU received\n"); fflush(stdout);
                }
                if (tray && tray->hmenu) {
                    POINT pt;
                    GetCursorPos(&pt);
                    SetForegroundWindow(hwnd);
                    // Enable/disable Stop menu item based on running state
                    if (ATOMIC_LOAD_BOOL(&tray->is_running) || ATOMIC_LOAD_BOOL(&tray->delayed_sleep_countdown_active)) {
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
