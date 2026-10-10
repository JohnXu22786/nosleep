#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()


def extract_function(name):
    definition = re.search(
        r"\bstatic\s+void\s+" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
        tray,
        re.S,
    )
    assert definition, f"could not find definition for {name}"
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
    raise AssertionError(f"unterminated function: {name}")


refresh = extract_function("refresh_notification_group_list")
update_actions = extract_function("update_notification_group_actions")
notifications_tab = extract_function("create_notifications_tab")

assert re.search(
    r'"LISTBOX", NULL,\s*WS_CHILD[^,]*\bWS_HSCROLL\b', notifications_tab, re.S
), "the notification group list must allow horizontal scrolling"
assert '"Up to 20 groups; Add Group is disabled at the limit.\\n"' in notifications_tab, (
    "the Notifications tab must explain why Add Group is disabled at capacity"
)
assert re.search(
    r"case WM_DPICHANGED:\s*scale_dialog_layout\(hwnd, HIWORD\(wParam\), \(const RECT\*\)lParam\);\s*"
    r"refresh_notification_group_list\(hNotifyTab, settings_tray\);",
    tray,
), "the list extent must be recalculated when its font changes with the dialog DPI"

harness = r'''#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void* HWND;
typedef void* HDC;
typedef void* HFONT;
typedef void* HGDIOBJ;
typedef unsigned int UINT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef int BOOL;
typedef struct { long cx, cy; } SIZE;

typedef struct {
    char name[128];
    bool is_default;
} NotifyGroup;

typedef struct {
    NotifyGroup groups[20];
    int count;
    int active_index;
} NotifyGroupManager;

typedef struct {
    NotifyGroupManager notify_groups;
} NoSleepTray;

#define IDC_NOTIFY_GROUP_LIST 501
#define IDC_NOTIFY_CONFIGURE_GROUP 502
#define IDC_NOTIFY_DEL_GROUP 503
#define IDC_NOTIFY_RESTORE_DEFAULT 504
#define IDC_NOTIFY_SET_ACTIVE 505
#define IDC_NOTIFY_ADD_GROUP 506
#define MAX_NOTIFY_GROUPS 20
#define LB_ERR (-1)
#define LB_GETCOUNT 1u
#define LB_GETCURSEL 2u
#define LB_GETITEMDATA 3u
#define LB_RESETCONTENT 4u
#define LB_ADDSTRING 5u
#define LB_SETITEMDATA 6u
#define LB_SETCURSEL 7u
#define LB_SETHORIZONTALEXTENT 8u
#define WM_GETFONT 9u

static HWND list_handle = (HWND)(uintptr_t)1;
static int list_count;
static int list_selection = LB_ERR;
static int list_horizontal_extent;
static int list_item_data[20];
static char list_item_text[20][256];
static bool control_enabled[600];
static int failures;

static int measure_test_text(const char* text, int count) {
    int width = 0;
    for (int i = 0; i < count; ++i) {
        width += text[i] == 'W' ? 11 : (text[i] == ' ' ? 4 : 8);
    }
    return width;
}

static HDC GetDC(HWND hwnd) {
    return hwnd == list_handle ? (HDC)(uintptr_t)2 : NULL;
}

static int ReleaseDC(HWND hwnd, HDC dc) {
    return hwnd == list_handle && dc != NULL;
}

static HGDIOBJ SelectObject(HDC dc, HGDIOBJ object) {
    return dc && object ? (HGDIOBJ)(uintptr_t)3 : NULL;
}

static BOOL GetTextExtentPoint32A(HDC dc, const char* text, int count, SIZE* size) {
    if (!dc || !text || count < 0 || !size) return 0;
    size->cx = measure_test_text(text, count);
    size->cy = 16;
    return 1;
}

static HWND GetDlgItem(HWND parent, int id) {
    (void)parent;
    return id == IDC_NOTIFY_GROUP_LIST ? list_handle : (HWND)(uintptr_t)id;
}

static int EnableWindow(HWND hwnd, int enable) {
    int id = (int)(uintptr_t)hwnd;
    bool was_enabled = id >= 0 && id < (int)(sizeof(control_enabled) / sizeof(control_enabled[0]))
        ? control_enabled[id]
        : false;
    if (id >= 0 && id < (int)(sizeof(control_enabled) / sizeof(control_enabled[0]))) {
        control_enabled[id] = enable != 0;
    }
    return was_enabled;
}

static bool is_control_enabled(int id) {
    return id >= 0 && id < (int)(sizeof(control_enabled) / sizeof(control_enabled[0]))
        ? control_enabled[id]
        : false;
}

static LRESULT SendMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (hwnd != list_handle) return LB_ERR;
    int index = (int)wParam;
    switch (message) {
        case WM_GETFONT:
            return (LRESULT)(uintptr_t)4;
        case LB_GETCOUNT:
            return list_count;
        case LB_GETCURSEL:
            return list_selection;
        case LB_GETITEMDATA:
            return index >= 0 && index < list_count ? list_item_data[index] : LB_ERR;
        case LB_RESETCONTENT:
            list_count = 0;
            list_selection = LB_ERR;
            return 0;
        case LB_ADDSTRING:
            if (list_count >= 20) return LB_ERR;
            snprintf(list_item_text[list_count], sizeof(list_item_text[list_count]), "%s", (const char*)lParam);
            list_item_data[list_count] = LB_ERR;
            return list_count++;
        case LB_SETITEMDATA:
            if (index < 0 || index >= list_count) return LB_ERR;
            list_item_data[index] = (int)lParam;
            return 0;
        case LB_SETCURSEL:
            if (index < 0 || index >= list_count) {
                list_selection = LB_ERR;
            } else {
                list_selection = index;
            }
            return list_selection;
        case LB_SETHORIZONTALEXTENT:
            list_horizontal_extent = index;
            return 0;
        default:
            return LB_ERR;
    }
}

static void seed_list(int count, int selected_row) {
    list_count = count;
    list_selection = selected_row;
    for (int i = 0; i < count; ++i) {
        list_item_data[i] = i;
    }
}

static void expect_selected_group(int expected_group, const char* message) {
    bool selected = list_selection >= 0 && list_selection < list_count &&
        list_item_data[list_selection] == expected_group;
    if (!selected) {
        fprintf(stderr, "FAIL: %s (selected row %d, expected group %d)\n",
                message, list_selection, expected_group);
        ++failures;
    }
}

'''

main = r'''int main(void) {
    NoSleepTray tray = {0};
    HWND parent = (HWND)(uintptr_t)2;

    // With no prior selection, the active group is the useful default.
    tray.notify_groups.count = 3;
    tray.notify_groups.active_index = 2;
    snprintf(tray.notify_groups.groups[0].name, sizeof(tray.notify_groups.groups[0].name), "All notifications");
    snprintf(tray.notify_groups.groups[1].name, sizeof(tray.notify_groups.groups[1].name), "Critical only");
    memset(tray.notify_groups.groups[2].name, 'W', sizeof(tray.notify_groups.groups[2].name) - 1);
    tray.notify_groups.groups[2].name[sizeof(tray.notify_groups.groups[2].name) - 1] = '\0';
    tray.notify_groups.groups[2].is_default = true;
    seed_list(0, LB_ERR);
    refresh_notification_group_list(parent, &tray);
    expect_selected_group(2, "initial refresh selects the active group");
    if (strcmp(list_item_text[2] + strlen(list_item_text[2]) - strlen(" [ACTIVE] (default)"),
               " [ACTIVE] (default)") != 0 ||
        list_horizontal_extent < measure_test_text(list_item_text[2], (int)strlen(list_item_text[2])) + 8) {
        fprintf(stderr, "FAIL: the horizontal extent includes the full rendered long label and suffixes\n");
        ++failures;
    }

    // Rebuilding an unchanged list must retain the row being configured or activated.
    tray.notify_groups.active_index = 0;
    seed_list(3, 1);
    refresh_notification_group_list(parent, &tray);
    expect_selected_group(1, "refresh preserves a still-existing selected group");

    // Appending a group leaves the old group's array index stable.
    tray.notify_groups.count = 4;
    snprintf(tray.notify_groups.groups[3].name, sizeof(tray.notify_groups.groups[3].name), "Custom");
    seed_list(3, 2);
    refresh_notification_group_list(parent, &tray);
    expect_selected_group(2, "adding a group preserves the prior selection");

    // After deleting a row, the active group is a valid fallback for the stale selection.
    tray.notify_groups.count = 3;
    tray.notify_groups.active_index = 1;
    seed_list(4, 3);
    refresh_notification_group_list(parent, &tray);
    expect_selected_group(1, "deleting the selected group falls back to active");

    // Add Group remains available below capacity and is disabled at the limit.
    tray.notify_groups.count = MAX_NOTIFY_GROUPS - 1;
    seed_list(tray.notify_groups.count, 0);
    update_notification_group_actions(parent, &tray);
    if (!is_control_enabled(IDC_NOTIFY_ADD_GROUP)) {
        fprintf(stderr, "FAIL: Add Group stays enabled below capacity\n");
        ++failures;
    }

    tray.notify_groups.count = MAX_NOTIFY_GROUPS;
    seed_list(tray.notify_groups.count, 0);
    update_notification_group_actions(parent, &tray);
    if (is_control_enabled(IDC_NOTIFY_ADD_GROUP)) {
        fprintf(stderr, "FAIL: Add Group is disabled at capacity\n");
        ++failures;
    }

    // Keep Delete unavailable when it would remove the final custom group.
    tray.notify_groups.count = 1;
    tray.notify_groups.active_index = 0;
    tray.notify_groups.groups[0].is_default = false;
    seed_list(1, 0);
    update_notification_group_actions(parent, &tray);
    if (is_control_enabled(IDC_NOTIFY_DEL_GROUP)) {
        fprintf(stderr, "FAIL: Delete is enabled for the sole custom group\n");
        ++failures;
    }

    // Removing one group is still available when another group remains.
    tray.notify_groups.count = 2;
    tray.notify_groups.active_index = 0;
    tray.notify_groups.groups[1].is_default = false;
    seed_list(2, 1);
    update_notification_group_actions(parent, &tray);
    if (!is_control_enabled(IDC_NOTIFY_DEL_GROUP)) {
        fprintf(stderr, "FAIL: Delete is disabled when another group can remain\n");
        ++failures;
    }

    return failures ? 1 : 0;
}
'''

with tempfile.TemporaryDirectory(prefix="nosleep-notify-selection-") as temp_dir:
    temp = Path(temp_dir)
    harness_path = temp / "notify_group_selection.c"
    executable_path = temp / "notify_group_selection"
    harness_path.write_text(harness + update_actions + refresh + main)
    subprocess.run(
        ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", str(harness_path), "-o", str(executable_path)],
        check=True,
    )
    subprocess.run([str(executable_path)], check=True)

print("PASS: notification group list preserves selection and measures long labels")
PY
