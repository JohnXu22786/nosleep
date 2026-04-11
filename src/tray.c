// System tray implementation for nosleep
#include "tray.h"
#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

NoSleepTray* tray_create() {
    DEBUG_PRINT("tray_create: allocating tray structure\n");
    NoSleepTray* tray = (NoSleepTray*)malloc(sizeof(NoSleepTray));
    if (!tray) return NULL;
    
    memset(tray, 0, sizeof(NoSleepTray));
    tray->duration_minutes = -1; // Not set
    tray->current_number = -1;   // No numbered icon displayed
    tray->prevent_display = false;
    tray->away_mode = false;
    tray->verbose = false;
    
    tray->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!tray->stop_event) {
        free(tray);
        return NULL;
    }
    
    return tray;
}

void tray_destroy(NoSleepTray* tray) {
    if (!tray) return;
    
    tray_stop_nosleep(tray, false);
    
    if (tray->stop_event) {
        CloseHandle(tray->stop_event);
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
        if (debug && (strcmp(debug, "1") == 0 || strcmp(debug, "2") == 0)) {
            SetTimer(tray->hwnd, TIMER_ID_AUTO_START, 1000, NULL);
        }
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
    printf("create_numbered_icon(%d)\n", number); fflush(stdout);
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
    char text[8];
    sprintf(text, "%d", number);
    
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

static void tray_create_icons(NoSleepTray* tray) {
    printf("tray_create_icons called\n"); fflush(stdout);
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_create_icons: creating default and active icons\n");
    }
    // Create custom colored icons
    tray->hIconDefault = create_colored_icon(RGB(128, 128, 128), true);      // Gray with Z
    tray->hIconActive = create_colored_icon(RGB(0, 128, 0), true);           // Green with Z
    
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
}

static void tray_create_menu(NoSleepTray* tray) {
    tray->hmenu = CreatePopupMenu();
    printf("[nosleep] tray_create_menu: CreatePopupMenu returned %p\n", tray->hmenu); fflush(stdout);
    if (!tray->hmenu) {
        printf("[nosleep] tray_create_menu: Failed to create menu (error %lu)\n", GetLastError()); fflush(stdout);
        return;
    }
    
    // Duration submenu
    HMENU hSubMenu = CreatePopupMenu();
    printf("[nosleep] tray_create_menu: CreatePopupMenu submenu returned %p\n", hSubMenu); fflush(stdout);
    if (!hSubMenu) {
        printf("[nosleep] tray_create_menu: Failed to create submenu (error %lu)\n", GetLastError()); fflush(stdout);
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
    
    AppendMenu(tray->hmenu, MF_STRING | MF_POPUP, (UINT_PTR)hSubMenu, "Set Duration");
    AppendMenu(tray->hmenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(tray->hmenu, MF_STRING, IDM_STOP, "Stop");
    AppendMenu(tray->hmenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(tray->hmenu, MF_STRING, IDM_EXIT, "Exit");
    printf("[nosleep] tray_create_menu: Menu created successfully\n"); fflush(stdout);
}

void tray_start_nosleep(NoSleepTray* tray, int duration_minutes) {
    printf("tray_start_nosleep(%d)\n", duration_minutes); fflush(stdout);
    if (!tray || tray->is_running) {
        return;
    }
    
    tray->duration_minutes = duration_minutes;
    tray->is_running = true;
    tray->duration_expired = false;
    tray->stopping = false;
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
    }
    
    // Start duration timer thread if duration is set
    if (duration_minutes > 0) {
        tray->timer_thread = CreateThread(
            NULL, 0, tray_duration_timer, tray, 0, NULL
        );
        if (tray->timer_thread) {
            tray->timer_thread_id = GetThreadId(tray->timer_thread);
        }
    }
}

void tray_stop_nosleep(NoSleepTray* tray, bool timer_expired) {
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_stop_nosleep called with timer_expired=%s\n", timer_expired ? "true" : "false");
    }
    
    // Prevent re-entrant calls
    if (!tray || tray->stopping) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_stop_nosleep: already stopping or invalid tray, returning\n");
        }
        return;
    }
    
    if (!tray->is_running) {
        return;
    }
    
    tray->stopping = true;
    tray->is_running = false;
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
        fprintf(stderr, "[nosleep] tray_stop_nosleep: timer_expired=%s, duration_expired=%s\n", timer_expired ? "true" : "false", tray->duration_expired ? "true" : "false");
    }
    
    // Determine which notification to show based on duration_expired flag
    if (tray->duration_expired) {
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
    
    // Update icon
    tray_update_icon(tray);
}

static DWORD WINAPI tray_duration_timer(LPVOID lpParam) {
    NoSleepTray* tray = (NoSleepTray*)lpParam;
    
    if (tray->duration_minutes <= 0) {
        return 0;
    }
    
    DWORD duration_ms = tray->duration_minutes * 60 * 1000;
    DWORD start_tick = GetTickCount();
    
    while (tray->is_running) {
        DWORD elapsed = GetTickCount() - start_tick;
        if (elapsed >= duration_ms) {
            // Duration reached
            const char* debug = getenv("NOSLEEP_DEBUG");
            if (debug && strcmp(debug, "1") == 0) {
                fprintf(stderr, "[nosleep] tray_duration_timer: duration reached, stopping with timer_expired=true\n");
            }
            tray->duration_expired = true;
            tray_stop_nosleep(tray, true);
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
        tray->duration_minutes > 0 ? tray->duration_minutes : 0, // 0 = indefinite
        20, // interval_seconds
        tray->prevent_display, // prevent_display
        tray->away_mode, // away_mode
        tray->verbose  // verbose
    );
    
    nosleep_destroy(ns);
    
    // If nosleep completed (duration reached or error), update tray state
    const char* debug = getenv("NOSLEEP_DEBUG");
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_nosleep_thread: nosleep completed, tray->is_running=%s\n", tray->is_running ? "true" : "false");
    }
    if (tray->is_running) {
        if (debug && strcmp(debug, "1") == 0) {
            fprintf(stderr, "[nosleep] tray_nosleep_thread: calling tray_stop_nosleep with timer_expired=false\n");
        }
        tray_stop_nosleep(tray, false);
    }
    
    return result;
}

void tray_update_icon(NoSleepTray* tray) {
    printf("tray_update_icon called\n"); fflush(stdout);
    const char* debug = getenv("NOSLEEP_DEBUG");
    int remaining_minutes = -1;
    if (!tray || !tray->hwnd) return;
    
    if (tray->is_running) {
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
            
            // Update icon with number
            if (remaining_minutes != tray->current_number) {
                if (debug && strcmp(debug, "1") == 0) {
                    fprintf(stderr, "[nosleep] tray_update_icon: remaining_minutes=%d current_number=%d\n", remaining_minutes, tray->current_number);
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
                if (remaining_minutes >= 0 && remaining_minutes < 60) {
                    // Use cached icon
                    if (tray->hIconNumbered[remaining_minutes] == NULL) {
                        tray->hIconNumbered[remaining_minutes] = create_numbered_icon(remaining_minutes);
                    }
                    numbered_icon = tray->hIconNumbered[remaining_minutes];
                } else {
                    // Create new icon for numbers >=60
                    numbered_icon = create_numbered_icon(remaining_minutes);
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
                tray->current_number = remaining_minutes;
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
                // If not cached (0-59), destroy it
                if (tray->current_number < 0 || tray->current_number >= 60) {
                    DestroyIcon(tray->hIconCurrentNumbered);
                }
                tray->hIconCurrentNumbered = NULL;
                tray->current_number = -1;
            }
            tray->nid.hIcon = tray->hIconActive;
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
    BOOL success = Shell_NotifyIcon(NIM_MODIFY, &tray->nid);
    if (debug && strcmp(debug, "1") == 0) {
        fprintf(stderr, "[nosleep] tray_update_icon: Shell_NotifyIcon %s\n", success ? "succeeded" : "failed");
        fflush(stderr);
    }
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

static int tray_show_custom_dialog(NoSleepTray* tray) {
    // Allocate a console for input
    if (!AllocConsole()) {
        // Console already allocated or error
        // Try to attach to existing console
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            MessageBox(tray->hwnd, "Cannot open console for input", "nosleep", MB_OK);
            return -1;
        }
    }
    
    FILE* f_in = freopen("CONIN$", "r", stdin);
    FILE* f_out = freopen("CONOUT$", "w", stdout);
    FILE* f_err = freopen("CONOUT$", "w", stderr);
    
    printf("\n");
    printf("==================================================\n");
    printf("nosleep - Custom Duration\n");
    printf("==================================================\n");
    printf("Enter the number of minutes to prevent system sleep.\n");
    printf("Valid range: 1 to 1440 minutes (24 hours)\n");
    printf("Enter 'cancel' to cancel.\n");
    printf("==================================================\n");
    
    while (1) {
        printf("\nDuration in minutes: ");
        fflush(stdout);
        
        char input[256];
        if (!fgets(input, sizeof(input), stdin)) {
            printf("Input cancelled.\n");
            break;
        }
        
        // Remove trailing newline
        input[strcspn(input, "\n")] = '\0';
        
        // Check for cancel
        if (strcmp(input, "cancel") == 0 || 
            strcmp(input, "quit") == 0 || 
            strcmp(input, "exit") == 0) {
            printf("Cancelled.\n");
            tray_show_notification(tray, "Cancelled", "Custom duration input cancelled");
            break;
        }
        
        // Parse integer
        char* endptr;
        long minutes = strtol(input, &endptr, 10);
        if (endptr == input || *endptr != '\0') {
            printf("Error: Please enter a valid number (1-1440).\n");
            continue;
        }
        
        if (minutes >= 1 && minutes <= 1440) {
            printf("Set to %ld minutes.\n", minutes);
            
            // Close console
            fclose(f_in);
            fclose(f_out);
            fclose(f_err);
            FreeConsole();
            
            return (int)minutes;
        } else {
            printf("Error: %ld is not in range 1-1440. Please try again.\n", minutes);
        }
    }
    
    // Close console
    fclose(f_in);
    fclose(f_out);
    fclose(f_err);
    FreeConsole();
    
    return -1; // cancelled
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
        printf("[nosleep] tray_window_proc: Tray message received (msg=0x%X) wParam=%lld, lParam=%lld (%s)\n", msg, wParam, lParam, msg_name); fflush(stdout);
        // Decode high and low words of lParam for debugging
        printf("[nosleep] tray_window_proc: lParam low word=0x%04X, high word=0x%04X\n", LOWORD(lParam), HIWORD(lParam)); fflush(stdout);
        
        // Handle right-click (both legacy mouse message and notification code)
        if (lParam == WM_RBUTTONUP || lParam == NIN_KEYSELECT) {
            // Show context menu
            printf("[nosleep] tray_window_proc: Right-click detected, showing menu\n"); fflush(stdout);
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd); // Required for menu to disappear properly
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
    tray_stop_nosleep(tray, false);
                    break;
                case IDM_EXIT:
                    DestroyWindow(hwnd);
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
            printf("[nosleep] tray_window_proc: WM_CONTEXTMENU received\n"); fflush(stdout);
            if (tray && tray->hmenu) {
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                TrackPopupMenu(tray->hmenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                PostMessage(hwnd, WM_NULL, 0, 0);
            }
            break;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    
    return 0;
}
