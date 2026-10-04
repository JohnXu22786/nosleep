#include "tray_countdown_icon.h"

#include <stdio.h>

typedef struct TestIcon {
    int number;
    bool owned;
} TestIcon;

static TestIcon numbered_icons[2];
static int create_calls;
static int destroy_calls;
static int failures;

static HICON fake_create_numbered_icon(int number) {
    if (create_calls >= (int)(sizeof(numbered_icons) / sizeof(numbered_icons[0]))) {
        return NULL;
    }

    TestIcon *icon = &numbered_icons[create_calls++];
    icon->number = number;
    icon->owned = true;
    return icon;
}

static BOOL WINAPI fake_destroy_icon(HICON icon) {
    if (!icon || !icon->owned) {
        return FALSE;
    }

    icon->owned = false;
    ++destroy_calls;
    return TRUE;
}

static void expect(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

int main(void) {
    HICON icon_cache[TRAY_COUNTDOWN_NUMBERED_ICON_COUNT] = {0};
    TestIcon blank_icon = { .number = -1, .owned = false };
    TestIcon fallback_icon = { .number = -2, .owned = false };

    HICON visible_icon = tray_countdown_display_icon(
        icon_cache, 60, true, &blank_icon, &fallback_icon,
        fake_create_numbered_icon);
    HICON countdown_icon = visible_icon;
    expect(countdown_icon != NULL && countdown_icon->number == 60,
           "the first visible 60-second update creates and displays icon 60");
    expect(create_calls == 1 && destroy_calls == 0,
           "the first visible update creates one owned icon");

    for (int update = 0; update < 5; ++update) {
        visible_icon = tray_countdown_display_icon(
            icon_cache, 60, false, &blank_icon, &fallback_icon,
            fake_create_numbered_icon);
        expect(visible_icon == &blank_icon,
               "blink-off updates display the blank icon");

        visible_icon = tray_countdown_display_icon(
            icon_cache, 60, true, &blank_icon, &fallback_icon,
            fake_create_numbered_icon);
        expect(visible_icon == countdown_icon,
               "blink-on updates reuse the same owned 60-second icon");
    }
    expect(create_calls == 1 && destroy_calls == 0,
           "repeated blink updates do not accumulate or prematurely destroy icons");

    tray_destroy_countdown_icon_cache(icon_cache, fake_destroy_icon);
    expect(destroy_calls == 1 && !countdown_icon->owned,
           "tray teardown releases the cached countdown icon exactly once");
    expect(icon_cache[60] == NULL,
           "tray teardown clears the cached countdown handle");

    tray_destroy_countdown_icon_cache(icon_cache, fake_destroy_icon);
    expect(destroy_calls == 1,
           "repeated teardown does not destroy the same icon twice");

    if (failures != 0) return 1;
    puts("PASS: countdown blink icon reuse and teardown cleanup");
    return 0;
}
