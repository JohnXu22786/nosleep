#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PYTEST'
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()


def extract_function(signature):
    start = tray.index(signature)
    brace = tray.index("{", start)
    depth = 0
    for index in range(brace, len(tray)):
        if tray[index] == "{":
            depth += 1
        elif tray[index] == "}":
            depth -= 1
            if depth == 0:
                return tray[start:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


helpers = "\n\n".join(
    extract_function(signature)
    for signature in (
        "static BOOL dialog_window_bounds_for_dpi(",
        "static UINT fit_dialog_dpi(",
    )
)

harness = r"""
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef int BOOL;
typedef struct { int left, top, right, bottom; } RECT;
typedef RECT* LPRECT;

#define WINAPI
#define TRUE 1
#define FALSE 0

static int MulDiv(int value, int numerator, int denominator) {
    long long product = (long long)value * numerator;
    return (int)((product + denominator / 2) / denominator);
}

static void* GetModuleHandle(const char* name) {
    (void)name;
    return NULL;
}

static void* GetProcAddress(void* module, const char* name) {
    (void)module;
    (void)name;
    return NULL;
}

static BOOL AdjustWindowRectEx(RECT* rect, DWORD style, BOOL menu, DWORD ex_style) {
    (void)style;
    (void)menu;
    (void)ex_style;
    rect->left -= 8;
    rect->right += 8;
    rect->top -= 31;
    rect->bottom += 8;
    return TRUE;
}

__HELPERS__

static int fits(UINT layout_dpi, UINT frame_dpi, int available_width, int available_height) {
    RECT bounds;
    if (!dialog_window_bounds_for_dpi(480, 470, layout_dpi, frame_dpi, 0, 0, &bounds)) return 0;
    return bounds.right - bounds.left <= available_width &&
        bounds.bottom - bounds.top <= available_height;
}

static int check_fit(const char* label, UINT requested, int width, int height) {
    UINT fitted = fit_dialog_dpi(requested, 480, 470, width, height, 0, 0);
    if (!fitted || fitted > requested || !fits(fitted, requested, width, height)) {
        fprintf(stderr, "FAIL: %s did not fit the work area (dpi %u)\n", label, fitted);
        return 1;
    }
    if (fitted < requested && fits(fitted + 1, requested, width, height)) {
        fprintf(stderr, "FAIL: %s left avoidable space unused (dpi %u)\n", label, fitted);
        return 1;
    }

    int client_height = MulDiv(470, fitted, 96);
    int buttons_bottom = MulDiv(430 + 28, fitted, 96);
    if (buttons_bottom > client_height) {
        fprintf(stderr, "FAIL: %s clipped the bottom action buttons\n", label);
        return 1;
    }
    printf("PASS: %s uses fitting layout DPI %u\n", label, fitted);
    return 0;
}

int main(void) {
    int failed = 0;
    failed |= check_fit("high DPI on a small work area", 192, 780, 650);
    failed |= check_fit("height-constrained monitor", 192, 1200, 620);
    failed |= check_fit("unchanged scale on a large monitor", 192, 1300, 1100);
    return failed;
}
""".replace("__HELPERS__", helpers)

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "settings_dialog_work_area.c"
    binary = Path(tmp) / "settings_dialog_work_area"
    source.write_text(harness)
    subprocess.run(
        shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", str(source), "-o", str(binary)
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
PYTEST
