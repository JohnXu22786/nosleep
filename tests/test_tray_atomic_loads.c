#include <stdbool.h>
#include <stdio.h>
#include <windows.h>

/* Select the production header's non-GNU compiler branch for this test. */
#undef __GNUC__
#include "../src/tray.h"

int main(void) {
    volatile bool active = true;
    volatile int countdown = 37;

    if (!ATOMIC_LOAD_BOOL(&active)) {
        fputs("FAIL: bool load did not return the stored true value\n", stderr);
        return 1;
    }
    if (test_interlocked_compare_exchange8_calls != 1) {
        fputs("FAIL: bool load did not use InterlockedCompareExchange8\n", stderr);
        return 1;
    }

    if (ATOMIC_LOAD_INT(&countdown) != 37) {
        fputs("FAIL: integer load did not return the stored value\n", stderr);
        return 1;
    }
    if (test_interlocked_compare_exchange_calls != 1) {
        fputs("FAIL: integer load did not use InterlockedCompareExchange\n", stderr);
        return 1;
    }

    active = false;
    countdown = 0;
    if (ATOMIC_LOAD_BOOL(&active) || ATOMIC_LOAD_INT(&countdown) != 0) {
        fputs("FAIL: atomic loads did not return updated zero values\n", stderr);
        return 1;
    }
    if (test_interlocked_compare_exchange8_calls != 2 ||
        test_interlocked_compare_exchange_calls != 2) {
        fputs("FAIL: repeated loads did not use the Interlocked APIs\n", stderr);
        return 1;
    }

    puts("PASS: non-GNU tray atomic loads use Interlocked compare-exchange APIs");
    return 0;
}
