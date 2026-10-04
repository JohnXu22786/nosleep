#include "tray_numbered_icon.h"

#include <stdbool.h>
#include <stdio.h>

typedef struct TestIcon {
    int id;
} TestIcon;

static int destroy_calls;
static HICON last_destroyed_icon;
static int failures;

static BOOL WINAPI fake_destroy_icon(HICON icon) {
    ++destroy_calls;
    last_destroyed_icon = icon;
    return TRUE;
}

static void expect(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

int main(void) {
    TestIcon active_icon = { .id = -1 };
    TestIcon numbered_icon = { .id = 60 };
    TestIcon cached_icon = { .id = 30 };
    HICON current_icon = &active_icon;

    // This is the state after creating an uncached icon failed for number 60.
    tray_release_numbered_icon(&current_icon, 60, &active_icon,
                               fake_destroy_icon);
    expect(destroy_calls == 0,
           "changing numbers after a failed icon creation does not destroy the shared active icon");
    expect(current_icon == NULL,
           "releasing the failed-number fallback clears the current handle");

    current_icon = &numbered_icon;
    tray_release_numbered_icon(&current_icon, 60, &active_icon,
                               fake_destroy_icon);
    expect(destroy_calls == 1 && last_destroyed_icon == &numbered_icon,
           "changing from an owned uncached icon destroys that icon");

    current_icon = &cached_icon;
    tray_release_numbered_icon(&current_icon, 30, &active_icon,
                               fake_destroy_icon);
    expect(destroy_calls == 1,
           "changing from a cached icon leaves it owned by the cache");

    if (failures != 0) return 1;
    puts("PASS: numbered icon fallback ownership and cleanup");
    return 0;
}
