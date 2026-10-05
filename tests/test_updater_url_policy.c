#include "updater_url_policy.h"

#include <stdio.h>

static int test_http_asset_url_is_rejected(void) {
    if (updater_url_is_https(L"http://github.com/example/nosleep.exe")) {
        fprintf(stderr, "HTTP update asset URL was accepted\n");
        return 1;
    }
    return 0;
}

static int test_http_redirect_is_rejected(void) {
    if (updater_url_is_https(L"http://objects.example.net/nosleep.exe?signature=abc")) {
        fprintf(stderr, "HTTP redirect target was accepted\n");
        return 1;
    }
    return 0;
}

static int test_https_asset_and_redirect_url_is_accepted(void) {
    if (!updater_url_is_https(L"HTTPS://objects.example.net/nosleep.exe?signature=abc")) {
        fprintf(stderr, "HTTPS update URL was rejected\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_http_asset_url_is_rejected();
    failures += test_http_redirect_is_rejected();
    failures += test_https_asset_and_redirect_url_is_accepted();
    printf("updater URL policy tests: %d passed, %d failed\n",
           3 - failures, failures);
    return failures == 0 ? 0 : 1;
}
