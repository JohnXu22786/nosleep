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


function = extract_function("static HICON create_transparent_icon(void)")
harness = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char BYTE;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HWND;
typedef void *HICON;
typedef void *HBITMAP;
typedef void *HDC;

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
#define WHITENESS 0x00FF0062
#define DEBUG_LOG(...) ((void)0)

static int tray_icon_width = 2;
static int tray_icon_height = 2;
static HDC screen_dc = (HDC)(uintptr_t)1;
static HDC memory_dc = (HDC)(uintptr_t)2;
static HDC mask_dc = (HDC)(uintptr_t)3;
static HBITMAP color_bitmap = (HBITMAP)(uintptr_t)4;
static HBITMAP mask_bitmap = (HBITMAP)(uintptr_t)5;
static HBITMAP previous_bitmap = (HBITMAP)(uintptr_t)6;
static HICON fallback_icon = (HICON)(uintptr_t)7;
static DWORD color_pixels[4];
static int fail_mask_dc;
static int fail_selection;
static int fail_mask_fill;
static int compatible_dc_calls;
static int selection_calls;
static int fill_calls;
static int indirect_calls;
static int delete_dc_calls;
static int delete_object_calls;
static int release_dc_calls;
static int mask_filled;
static int failed;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failed = 1;
    }
}

static HICON CreateIcon(HWND instance, int width, int height, UINT planes,
                        UINT bits_per_pixel, const BYTE *and_bits,
                        const BYTE *xor_bits) {
    (void)instance;
    check(width == 2 && height == 2 && planes == 1 && bits_per_pixel == 1,
          "the primary icon attempt uses the requested size and format");
    check(and_bits != NULL && xor_bits != NULL,
          "the primary icon attempt receives initialized bitmaps");
    return NULL;
}

static HDC GetDC(HWND window) {
    check(window == NULL, "the fallback acquires the screen DC");
    return screen_dc;
}

static HDC CreateCompatibleDC(HDC dc) {
    check(dc == screen_dc, "compatible DCs use the screen DC");
    ++compatible_dc_calls;
    if (compatible_dc_calls == 1) return memory_dc;
    if (compatible_dc_calls == 2 && fail_mask_dc) return NULL;
    return mask_dc;
}

static HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *info, UINT usage,
                                void **bits, HANDLE section, DWORD offset) {
    (void)section;
    (void)offset;
    check(dc == screen_dc && usage == DIB_RGB_COLORS,
          "the color DIB uses the screen DC");
    check(info->bmiHeader.biWidth == 2 && info->bmiHeader.biHeight == -2 &&
              info->bmiHeader.biPlanes == 1 && info->bmiHeader.biBitCount == 32,
          "the color DIB uses the requested 32-bit dimensions");
    *bits = color_pixels;
    return color_bitmap;
}

static HBITMAP CreateBitmap(int width, int height, UINT planes,
                            UINT bits_per_pixel, const void *bits) {
    check(width == 2 && height == 2 && planes == 1 && bits_per_pixel == 1,
          "the fallback mask is a 1-bit bitmap of the requested size");
    check(bits == NULL, "the fallback mask begins without supplied pixel data");
    return mask_bitmap;
}

static HBITMAP SelectObject(HDC dc, HBITMAP bitmap) {
    check(dc == mask_dc, "the fallback mask DC selects the mask bitmap");
    ++selection_calls;
    if (selection_calls == 1) {
        check(bitmap == mask_bitmap, "the mask bitmap is selected for filling");
        return fail_selection ? NULL : previous_bitmap;
    }
    check(selection_calls == 2 && bitmap == previous_bitmap,
          "the original bitmap is restored after filling");
    return mask_bitmap;
}

static BOOL PatBlt(HDC dc, int x, int y, int width, int height, DWORD operation) {
    check(dc == mask_dc && x == 0 && y == 0 && width == 2 && height == 2 &&
              operation == WHITENESS,
          "the mask is filled white over the full icon");
    check(!fail_selection, "the mask is filled only after successful selection");
    ++fill_calls;
    if (fail_mask_fill) return FALSE;
    mask_filled = 1;
    return TRUE;
}

static HICON CreateIconIndirect(const ICONINFO *info) {
    ++indirect_calls;
    check(mask_filled, "CreateIconIndirect receives an initialized mask");
    check(info->fIcon == TRUE && info->hbmColor == color_bitmap &&
              info->hbmMask == mask_bitmap,
          "the fallback icon receives its color and mask bitmaps");
    return fallback_icon;
}

static int DeleteObject(HBITMAP bitmap) {
    check(bitmap == color_bitmap || bitmap == mask_bitmap,
          "the fallback releases its created bitmaps");
    ++delete_object_calls;
    return TRUE;
}

static int DeleteDC(HDC dc) {
    check(dc == memory_dc || dc == mask_dc,
          "the fallback releases its created DCs");
    ++delete_dc_calls;
    return TRUE;
}

static int ReleaseDC(HWND window, HDC dc) {
    check(window == NULL && dc == screen_dc,
          "the fallback releases the screen DC");
    ++release_dc_calls;
    return TRUE;
}

static inline DWORD GetLastError(void) { return 5; }

__ICON_FUNCTION__

static void run_case(int fail_dc, int fail_select, int fail_fill,
                     int expect_icon) {
    fail_mask_dc = fail_dc;
    fail_selection = fail_select;
    fail_mask_fill = fail_fill;
    compatible_dc_calls = 0;
    selection_calls = 0;
    fill_calls = 0;
    indirect_calls = 0;
    delete_dc_calls = 0;
    delete_object_calls = 0;
    release_dc_calls = 0;
    mask_filled = 0;
    failed = 0;

    HICON icon = create_transparent_icon();
    check((icon == fallback_icon) == expect_icon,
          "fallback result matches whether the mask was initialized");
    check(indirect_calls == expect_icon,
          "CreateIconIndirect runs only after successful mask initialization");
    check(delete_object_calls == 2,
          "both fallback bitmaps are released");
    check(delete_dc_calls == (fail_dc ? 1 : 2),
          "all successfully created fallback DCs are released");
    check(release_dc_calls == 1, "the screen DC is released");
    if (fail_dc) {
        check(selection_calls == 0 && fill_calls == 0,
              "mask DC creation failure prevents mask use");
    } else if (fail_select) {
        check(selection_calls == 1 && fill_calls == 0,
              "selection failure prevents mask use");
    } else {
        check(selection_calls == 2 && fill_calls == 1,
              "the mask is restored after one fill attempt");
    }
    if (failed) exit(1);
}

int main(void) {
    run_case(1, 0, 0, 0);
    run_case(0, 1, 0, 0);
    run_case(0, 0, 1, 0);
    run_case(0, 0, 0, 1);
    puts("PASS: fallback icons are created only with an initialized mask");
    return 0;
}
""".replace("__ICON_FUNCTION__", function)

with tempfile.TemporaryDirectory(prefix="nosleep-transparent-icon-mask-") as temp_dir:
    source = Path(temp_dir) / "test_tray_transparent_icon_mask.c"
    binary = Path(temp_dir) / "test_tray_transparent_icon_mask"
    source.write_text(harness)
    compiler = os.environ.get("CC", "cc")
    subprocess.run(
        [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
PY
