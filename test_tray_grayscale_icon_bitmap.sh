#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()


def extract_function(signature):
    start = tray.index(signature)
    opening = tray.index("{", start)
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
harness = r"""
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

#define TRUE 1
#define FALSE 0
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define DI_NORMAL 3
#define ZeroMemory(destination, size) memset((destination), 0, (size))
#define DEBUG_LOG(...) ((void)0)

static DWORD dib_pixels[2];
static DWORD captured_pixels[2];
static HBITMAP expected_mask = (HBITMAP)(uintptr_t)2;
static int transparent_slot_zero_before_draw;
static int captured_icon;
static int failed;
static int select_calls;

static HICON input_icon = (HICON)(uintptr_t)3;
static HBITMAP color_bitmap = (HBITMAP)(uintptr_t)4;
static HBITMAP dib_bitmap = (HBITMAP)(uintptr_t)5;
static HBITMAP old_bitmap = (HBITMAP)(uintptr_t)6;
static HDC screen_dc = (HDC)(uintptr_t)7;
static HDC memory_dc = (HDC)(uintptr_t)8;

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
    transparent_slot_zero_before_draw = dib_pixels[1] == 0;
    dib_pixels[0] = 0xFF112233;
    return TRUE;
}

static BOOL GdiFlush(void) { return TRUE; }

static HICON CreateIconIndirect(ICONINFO *info) {
    check(info->fIcon == TRUE, "grayscale result is an icon");
    check(info->hbmColor == dib_bitmap, "grayscale DIB is used as the icon color bitmap");
    check(info->hbmMask == expected_mask, "the original icon mask is preserved");
    memcpy(captured_pixels, dib_pixels, sizeof(captured_pixels));
    captured_icon = 1;
    return (HICON)(uintptr_t)9;
}

static BOOL DeleteObject(HBITMAP bitmap) {
    (void)bitmap;
    return TRUE;
}

static BOOL DeleteDC(HDC dc) {
    check(dc == memory_dc, "memory DC is deleted");
    return TRUE;
}

static int ReleaseDC(HWND window, HDC dc) {
    check(window == NULL && dc == screen_dc, "screen DC is released");
    return 1;
}

__ICON_FUNCTION__

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
    if (failed) return 1;
    puts("PASS: grayscale icon conversion clears unwritten pixels and preserves transparency");
    return 0;
}
""".replace("__ICON_FUNCTION__", function)

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
