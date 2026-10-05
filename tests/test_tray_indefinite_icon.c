#include "tray_indefinite_icon.h"

#include <stdbool.h>
#include <stdio.h>

typedef struct TestIcon {
    int id;
} TestIcon;

static TestIcon icons[8];
static int create_calls;
static int successful_creates;
static int destroy_calls;
static HICON last_destroyed_icon;
static bool fail_next_create;
static bool created_wrong_number;
static int expected_failed_number = -1;
static int failures;

static HICON fake_create_numbered_icon(int number) {
    ++create_calls;
    if (number != -1 && number != expected_failed_number) {
        created_wrong_number = true;
    }
    if (fail_next_create) {
        fail_next_create = false;
        return NULL;
    }
    icons[successful_creates].id = successful_creates;
    return &icons[successful_creates++];
}

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
    HICON current_icon = NULL;
    int current_number = -1;

    tray_ensure_indefinite_icon(&current_icon, &current_number, &active_icon,
                                fake_create_numbered_icon, fake_destroy_icon);
    HICON infinity_icon = current_icon;
    expect(infinity_icon != NULL && current_number == -1,
           "an indefinite session creates the infinity icon");
    expect(create_calls == 1 && destroy_calls == 0,
           "the first indefinite update creates one icon without destroying any");

    tray_ensure_indefinite_icon(&current_icon, &current_number, &active_icon,
                                fake_create_numbered_icon, fake_destroy_icon);
    expect(current_icon == infinity_icon,
           "repeated indefinite updates preserve the existing icon handle");
    expect(create_calls == 1 && destroy_calls == 0,
           "repeated indefinite updates do not recreate or destroy the icon");

    current_icon = &icons[6];
    current_number = 30;
    tray_ensure_indefinite_icon(&current_icon, &current_number, &active_icon,
                                fake_create_numbered_icon, fake_destroy_icon);
    expect(current_icon == &icons[1] && current_number == -1,
           "switching from a cached number creates the infinity icon");
    expect(destroy_calls == 0,
           "switching from a cached number leaves the cached icon owned by its cache");

    current_icon = &icons[7];
    current_number = 60;
    tray_ensure_indefinite_icon(&current_icon, &current_number, &active_icon,
                                fake_create_numbered_icon, fake_destroy_icon);
    expect(current_icon == &icons[2] && current_number == -1,
           "switching from an uncached number creates the infinity icon");
    expect(destroy_calls == 1 && last_destroyed_icon == &icons[7],
           "switching from an uncached number releases the old icon");

    fail_next_create = true;
    current_icon = NULL;
    current_number = -1;
    tray_ensure_indefinite_icon(&current_icon, &current_number, &active_icon,
                                fake_create_numbered_icon, fake_destroy_icon);
    expect(current_icon == NULL && current_number == -1,
           "failed icon creation leaves the handle available for retry");
    tray_ensure_indefinite_icon(&current_icon, &current_number, &active_icon,
                                fake_create_numbered_icon, fake_destroy_icon);
    expect(current_icon == &icons[3] && create_calls == 5,
           "a missing infinity icon is retried on the next update");

    fail_next_create = true;
    expected_failed_number = 65;
    HICON failed_numbered_icon = fake_create_numbered_icon(65);
    expected_failed_number = -1;
    expect(failed_numbered_icon == NULL,
           "an uncached numbered icon can fail creation");
    current_icon = failed_numbered_icon ? failed_numbered_icon : &active_icon;
    current_number = 65;
    int destroys_before_fallback_transition = destroy_calls;
    tray_ensure_indefinite_icon(&current_icon, &current_number, &active_icon,
                                fake_create_numbered_icon, fake_destroy_icon);
    expect(destroy_calls == destroys_before_fallback_transition,
           "transitioning after failed uncached creation does not destroy the shared active icon");
    expect(current_icon == &icons[4] && current_number == -1,
           "failed-number fallback transitions to the infinity icon");
    expect(!created_wrong_number, "infinite sessions request the -1 icon");

    if (failures != 0) return 1;
    puts("PASS: indefinite icon creation, reuse, transition cleanup, and retry");
    return 0;
}
