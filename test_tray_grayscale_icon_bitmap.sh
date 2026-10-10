#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()


def extract_function(signature):
    definition = re.search(re.escape(signature) + r"\s*\{", tray)
    assert definition, f"could not find definition for {signature}"
    start = definition.start()
    opening = definition.end() - 1
    depth = 0
    state = "code"
    i = opening
    while i < len(tray):
        char = tray[i]
        next_two = tray[i : i + 2]
        if state == "code":
            if next_two == "//":
                state = "line_comment"
                i += 2
                continue
            if next_two == "/*":
                state = "block_comment"
                i += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return tray[start : i + 1]
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if next_two == "*/":
                state = "code"
                i += 2
                continue
        elif state in ("string", "char"):
            if char == "\\":
                i += 2
                continue
            if (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        i += 1
    raise AssertionError(f"unterminated function: {signature}")


function = extract_function("static HICON icon_to_grayscale(HICON hColorIcon)")
caller = extract_function("static void load_gray_and_color_icons(NoSleepTray* tray)")
harness = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char BYTE;
typedef uint32_t DWORD;
typedef int BOOL;
typedef void *HICON;
typedef void *HBITMAP;
typedef void *HDC;
typedef void *HBRUSH;
typedef void *HANDLE;
typedef void *HWND;
typedef unsigned int UINT;
typedef unsigned long COLORREF;
typedef const char *LPCTSTR;

typedef struct {
    int bmWidth;
    int bmHeight;
} BITMAP;

typedef struct {
    DWORD biSize;
    int biWidth;
    int biHeight;
    unsigned short biPlanes;
    unsigned short biBitCount;
    DWORD biCompression;
} BITMAPINFOHEADER;

typedef struct {
    BITMAPINFOHEADER bmiHeader;
} BITMAPINFO;

typedef struct {
    BOOL fIcon;
    HBITMAP hbmColor;
    HBITMAP hbmMask;
} ICONINFO;

typedef struct {
    HICON hIconActive;
    HICON hIconDefault;
} NoSleepTray;

#define TRUE 1
#define FALSE 0
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define DI_NORMAL 3
#define IDI_APPICON 101
#define MAKEINTRESOURCE(value) ((LPCTSTR)(uintptr_t)(value))
#define RGB(red, green, blue) ((COLORREF)((red) | ((green) << 8) | ((blue) << 16)))
#define ZeroMemory(destination, size) memset((destination), 0, (size))
#define DEBUG_LOG(...) ((void)0)

static int tray_icon_width = 16;
static int tray_icon_height = 16;
static DWORD dib_pixels[2];
static DWORD captured_pixels[2];
static HBITMAP expected_mask = (HBITMAP)(uintptr_t)2;
static int transparent_slot_zero_before_draw;
static int captured_icon;
static int create_icon_calls;
static int failed;
static int select_calls;
static BOOL draw_succeeds = TRUE;
static int draw_calls;
static int delete_color_bitmap_calls;
static int delete_mask_bitmap_calls;
static int delete_dib_bitmap_calls;
static int delete_dc_calls;
static int release_dc_calls;
static int load_icon_calls;
static int colored_icon_calls;
static COLORREF colored_icon_color;
static bool colored_icon_draw_z;

static HICON input_icon = (HICON)(uintptr_t)3;
static HBITMAP color_bitmap = (HBITMAP)(uintptr_t)4;
static HBITMAP dib_bitmap = (HBITMAP)(uintptr_t)5;
static HBITMAP old_bitmap = (HBITMAP)(uintptr_t)6;
static HDC screen_dc = (HDC)(uintptr_t)7;
static HDC memory_dc = (HDC)(uintptr_t)8;
static HICON gray_fallback_icon = (HICON)(uintptr_t)10;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failed = 1;
    }
}

static BOOL GetIconInfo(HICON icon, ICONINFO *info) {
    check(icon == input_icon, "GetIconInfo receives the input icon");
    info->fIcon = TRUE;
    info->hbmColor = color_bitmap;
    info->hbmMask = expected_mask;
    return TRUE;
}

static HDC GetDC(HWND window) {
    check(window == NULL, "GetDC is called for the screen");
    return screen_dc;
}

static HDC CreateCompatibleDC(HDC dc) {
    check(dc == screen_dc, "memory DC is compatible with the screen DC");
    return memory_dc;
}

static int GetObject(HBITMAP bitmap, int size, void *object) {
    (void)size;
    check(bitmap == color_bitmap, "dimensions are read from the color icon bitmap");
    BITMAP *result = (BITMAP *)object;
    result->bmWidth = 2;
    result->bmHeight = 1;
    return (int)sizeof(*result);
}

static HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *info, UINT usage,
                                void **bits, HANDLE section, DWORD offset) {
    (void)section;
    (void)offset;
    check(dc == screen_dc && usage == DIB_RGB_COLORS,
          "DIB is created for the screen DC with RGB colors");
    check(info->bmiHeader.biWidth == 2 && info->bmiHeader.biHeight == -1,
          "DIB uses the input dimensions and a top-down layout");
    dib_pixels[0] = 0xFFEEDDCC;
    dib_pixels[1] = 0xFFEEDDCC;
    *bits = dib_pixels;
    return dib_bitmap;
}

static HBITMAP SelectObject(HDC dc, HBITMAP bitmap) {
    check(dc == memory_dc, "bitmap selection uses the memory DC");
    ++select_calls;
    if (select_calls == 1) {
        check(bitmap == dib_bitmap, "grayscale DIB is selected");
        return old_bitmap;
    }
    check(select_calls == 2 && bitmap == old_bitmap,
          "original bitmap is restored during cleanup");
    return dib_bitmap;
}

static BOOL DrawIconEx(HDC dc, int x, int y, HICON icon, int width, int height,
                       UINT step, HBRUSH brush, UINT flags) {
    (void)step;
    (void)brush;
    check(dc == memory_dc && x == 0 && y == 0 && icon == input_icon &&
              width == 2 && height == 1 && flags == DI_NORMAL,
          "the input icon is drawn to the grayscale DIB");
    ++draw_calls;
    transparent_slot_zero_before_draw = dib_pixels[1] == 0;
    if (!draw_succeeds) return FALSE;
    dib_pixels[0] = 0xFF112233;
    return TRUE;
}

static BOOL GdiFlush(void) { return TRUE; }

static HICON CreateIconIndirect(ICONINFO *info) {
    ++create_icon_calls;
    check(info->fIcon == TRUE, "grayscale result is an icon");
    check(info->hbmColor == dib_bitmap, "grayscale DIB is used as the icon color bitmap");
    check(info->hbmMask == expected_mask, "the original icon mask is preserved");
    memcpy(captured_pixels, dib_pixels, sizeof(captured_pixels));
    captured_icon = 1;
    return (HICON)(uintptr_t)9;
}

static BOOL DeleteObject(HBITMAP bitmap) {
    if (bitmap == color_bitmap) ++delete_color_bitmap_calls;
    else if (bitmap == expected_mask) ++delete_mask_bitmap_calls;
    else if (bitmap == dib_bitmap) ++delete_dib_bitmap_calls;
    else check(0, "only acquired icon and DIB bitmaps are deleted");
    return TRUE;
}

static BOOL DeleteDC(HDC dc) {
    check(dc == memory_dc, "memory DC is deleted");
    ++delete_dc_calls;
    return TRUE;
}

static int ReleaseDC(HWND window, HDC dc) {
    check(window == NULL && dc == screen_dc, "screen DC is released");
    ++release_dc_calls;
    return 1;
}

static HICON load_icon_from_resource(LPCTSTR resource_name, int width, int height) {
    check(resource_name == MAKEINTRESOURCE(IDI_APPICON),
          "caller loads the application icon resource");
    check(width == tray_icon_width && height == tray_icon_height,
          "caller requests the configured tray icon dimensions");
    ++load_icon_calls;
    return input_icon;
}

static HICON create_colored_icon(COLORREF color, bool draw_z) {
    ++colored_icon_calls;
    colored_icon_color = color;
    colored_icon_draw_z = draw_z;
    return gray_fallback_icon;
}

__ICON_FUNCTION__
__CALLER_FUNCTION__

int main(void) {
    HICON gray_icon = icon_to_grayscale(input_icon);
    check(gray_icon != NULL, "grayscale icon conversion succeeds");
    check(transparent_slot_zero_before_draw,
          "unwritten transparent DIB pixels are cleared before drawing");
    check(captured_icon, "CreateIconIndirect receives the grayscale bitmap");
    check((captured_pixels[0] >> 24) == 0xFF,
          "the drawn opaque pixel keeps its alpha");
    check(captured_pixels[0] == 0xFF1E1E1E,
          "the drawn opaque pixel is converted to grayscale");
    check(captured_pixels[1] == 0,
          "unwritten transparent pixels stay transparent in the grayscale icon");

    draw_succeeds = FALSE;
    select_calls = 0;
    create_icon_calls = 0;
    draw_calls = 0;
    delete_color_bitmap_calls = 0;
    delete_mask_bitmap_calls = 0;
    delete_dib_bitmap_calls = 0;
    delete_dc_calls = 0;
    release_dc_calls = 0;
    load_icon_calls = 0;
    colored_icon_calls = 0;
    NoSleepTray tray = {0};
    load_gray_and_color_icons(&tray);
    check(load_icon_calls == 1 && tray.hIconActive == input_icon,
          "caller keeps the loaded active icon after grayscale drawing fails");
    check(draw_calls == 1 && create_icon_calls == 0,
          "failed DrawIconEx does not create a grayscale icon");
    check(colored_icon_calls == 1 && colored_icon_color == RGB(128, 128, 128) &&
              colored_icon_draw_z,
          "caller creates the existing gray Z fallback after grayscale failure");
    check(tray.hIconDefault == gray_fallback_icon,
          "caller uses the generated gray fallback icon");
    check(select_calls == 2 && delete_dib_bitmap_calls == 1 &&
              delete_color_bitmap_calls == 1 && delete_mask_bitmap_calls == 1 &&
              delete_dc_calls == 1 && release_dc_calls == 1,
          "failed grayscale conversion restores selection and releases every acquired GDI resource");
    if (failed) return 1;
    puts("PASS: grayscale conversion preserves transparency and falls back when DrawIconEx fails");
    return 0;
}
""".replace("__ICON_FUNCTION__", function).replace("__CALLER_FUNCTION__", caller)

with tempfile.TemporaryDirectory(prefix="nosleep-grayscale-icon-") as temp_dir:
    source = Path(temp_dir) / "test_tray_grayscale_icon_bitmap.c"
    binary = Path(temp_dir) / "test_tray_grayscale_icon_bitmap"
    source.write_text(harness)
    compiler = os.environ.get("CC", "cc")
    subprocess.run(
        [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
PY
