#include "../src/updater_redirect.h"

#include <stdio.h>
#include <wchar.h>

static int test_request_target_preserves_query_and_omits_fragment(void) {
    static const wchar_t path[] = L"/release/nosleep.exe";
    static const wchar_t extra_info[] = L"?token=abc&signature=xyz#client-fragment";
    static const wchar_t expected[] = L"/release/nosleep.exe?token=abc&signature=xyz";
    wchar_t target[128];

    if (!updater_build_request_target(path, extra_info, target,
                                      sizeof(target) / sizeof(target[0]))) {
        fprintf(stderr, "failed to build request target\n");
        return 1;
    }
    if (wcscmp(target, expected) != 0) {
        fprintf(stderr, "request target did not preserve the query or omit the fragment\n");
        return 1;
    }
    return 0;
}

static int test_request_target_without_query_keeps_path(void) {
    static const wchar_t path[] = L"/release/nosleep.exe";
    wchar_t target[64];

    if (!updater_build_request_target(path, L"", target,
                                      sizeof(target) / sizeof(target[0]))) {
        fprintf(stderr, "failed to build request target without query\n");
        return 1;
    }
    if (wcscmp(target, path) != 0) {
        fprintf(stderr, "request target changed a path without query info\n");
        return 1;
    }
    return 0;
}

static int test_request_target_with_fragment_only_keeps_path(void) {
    static const wchar_t path[] = L"/release/nosleep.exe";
    wchar_t target[64];

    if (!updater_build_request_target(path, L"#client-fragment", target,
                                      sizeof(target) / sizeof(target[0]))) {
        fprintf(stderr, "failed to build request target with fragment only\n");
        return 1;
    }
    if (wcscmp(target, path) != 0) {
        fprintf(stderr, "request target included a fragment without a query\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_request_target_preserves_query_and_omits_fragment();
    failures += test_request_target_without_query_keeps_path();
    failures += test_request_target_with_fragment_only_keeps_path();
    printf("updater request target tests: %d passed, %d failed\n",
           3 - failures, failures);
    return failures == 0 ? 0 : 1;
}
