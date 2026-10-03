#include "../src/updater_redirect.h"

#include <stdio.h>
#include <wchar.h>

static int test_redirect_target_preserves_signed_query(void) {
    static const wchar_t path[] = L"/release/nosleep.exe";
    static const wchar_t extra_info[] = L"?token=abc&signature=xyz";
    static const wchar_t expected[] = L"/release/nosleep.exe?token=abc&signature=xyz";
    wchar_t target[128];

    if (!updater_build_redirect_target(path, extra_info, target,
                                      sizeof(target) / sizeof(target[0]))) {
        fprintf(stderr, "failed to build redirect request target\n");
        return 1;
    }
    if (wcscmp(target, expected) != 0) {
        fprintf(stderr, "redirect request target did not preserve its signed query\n");
        return 1;
    }
    return 0;
}

static int test_redirect_target_without_query_keeps_path(void) {
    static const wchar_t path[] = L"/release/nosleep.exe";
    wchar_t target[64];

    if (!updater_build_redirect_target(path, L"", target,
                                      sizeof(target) / sizeof(target[0]))) {
        fprintf(stderr, "failed to build redirect request target without query\n");
        return 1;
    }
    if (wcscmp(target, path) != 0) {
        fprintf(stderr, "redirect request target changed a path without query info\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_redirect_target_preserves_signed_query();
    failures += test_redirect_target_without_query_keeps_path();
    printf("updater redirect target tests: %d passed, %d failed\n",
           2 - failures, failures);
    return failures == 0 ? 0 : 1;
}
