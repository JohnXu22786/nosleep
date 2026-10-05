// Test suite for updater module
// Tests JSON parsing and version comparison functions
// Tests production updater logic without linking the Windows-only updater transport.
#include "updater_logic.h"
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  TEST: %s ... ", name); fflush(stdout); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; fflush(stdout); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; fflush(stdout); } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

static int ends_with(const char* str, const char* suffix) {
    if (!str || !suffix) return 0;
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len) return 0;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

static bool parse_asset_url(const char* url, UpdateInfo* info) {
    char response[1024];
    int response_len = snprintf(response, sizeof(response),
        "{\"tag_name\":\"v2.1.0\",\"assets\":[{\"browser_download_url\":\"%s\"}]}", url);
    if (response_len < 0 || (size_t)response_len >= sizeof(response)) return false;
    return updater_parse_response(response, info);
}

// ---- Sample JSON data ----

static const char* SAMPLE_JSON_FULL =
    "{"
    "  \"url\": \"https://api.github.com/repos/JohnXu22786/nosleep/releases/latest\","
    "  \"tag_name\": \"v2.1.0\","
    "  \"name\": \"nosleep v2.1.0\","
    "  \"assets\": ["
    "    {"
    "      \"name\": \"nosleep-2.1.0.exe\","
    "      \"browser_download_url\": \"https://github.com/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep-2.1.0.exe\","
    "      \"content_type\": \"application/x-msdownload\""
    "    },"
    "    {"
    "      \"name\": \"nosleep-2.1.0.zip\","
    "      \"browser_download_url\": \"https://github.com/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep-2.1.0.zip\","
    "      \"content_type\": \"application/zip\""
    "    }"
    "  ]"
    "}";

static const char* SAMPLE_JSON_NO_EXE =
    "{"
    "  \"tag_name\": \"v2.1.0\","
    "  \"name\": \"nosleep v2.1.0\","
    "  \"assets\": ["
    "    {"
    "      \"name\": \"nosleep-2.1.0.zip\","
    "      \"browser_download_url\": \"https://github.com/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep-2.1.0.zip\""
    "    }"
    "  ]"
    "}";

static const char* SAMPLE_JSON_NO_TAG =
    "{"
    "  \"name\": \"nosleep v2.1.0\""
    "}";

static const char* SAMPLE_JSON_NO_ASSETS =
    "{"
    "  \"tag_name\": \"v2.1.0\""
    "}";

static const char* SAMPLE_JSON_EMPTY_ASSETS =
    "{"
    "  \"tag_name\": \"v2.1.0\","
    "  \"assets\": []"
    "}";

static const char* SAMPLE_JSON_NO_V =
    "{"
    "  \"tag_name\": \"2.1.0\","
    "  \"assets\": ["
    "    {"
    "      \"name\": \"nosleep-2.1.0.exe\","
    "      \"browser_download_url\": \"https://github.com/JohnXu22786/nosleep/releases/download/2.1.0/nosleep-2.1.0.exe\""
    "    }"
    "  ]"
    "}";

static const char* SAMPLE_JSON_INVALID = "{ invalid json }";

// ---- Test cases ----

static void test_ver_compare(void) {
    printf("\n--- updater_compare_versions ---\n");
    fflush(stdout);

    TEST("v1 > v2 (major)");
    ASSERT(updater_compare_versions("2.0.0", "1.9.9") == 1, "Expected 1");
    PASS();

    TEST("v1 < v2 (major)");
    ASSERT(updater_compare_versions("1.9.9", "2.0.0") == -1, "Expected -1");
    PASS();

    TEST("v1 > v2 (minor)");
    ASSERT(updater_compare_versions("1.2.0", "1.1.9") == 1, "Expected 1");
    PASS();

    TEST("v1 < v2 (minor)");
    ASSERT(updater_compare_versions("1.1.9", "1.2.0") == -1, "Expected -1");
    PASS();

    TEST("v1 > v2 (patch)");
    ASSERT(updater_compare_versions("1.1.2", "1.1.1") == 1, "Expected 1");
    PASS();

    TEST("v1 == v2");
    ASSERT(updater_compare_versions("1.1.1", "1.1.1") == 0, "Expected 0");
    PASS();

    TEST("v1 == v2 with leading v");
    ASSERT(updater_compare_versions("v1.1.1", "1.1.1") == 0, "Expected 0");
    PASS();

    TEST("v1 == v2 with leading V");
    ASSERT(updater_compare_versions("V1.1.1", "1.1.1") == 0, "Expected 0");
    PASS();

    TEST("stable release is greater than same-core prerelease");
    ASSERT(updater_compare_versions("1.2.3", "1.2.3-rc.1") == 1, "Expected 1");
    PASS();

    TEST("same-core prerelease is less than stable release");
    ASSERT(updater_compare_versions("1.2.3-rc.1", "1.2.3") == -1, "Expected -1");
    PASS();

    TEST("numeric prerelease identifiers compare numerically");
    ASSERT(updater_compare_versions("1.2.3-rc.2", "1.2.3-rc.10") == -1, "Expected -1");
    PASS();

    TEST("numeric prerelease identifier has lower precedence than nonnumeric");
    ASSERT(updater_compare_versions("1.2.3-1", "1.2.3-alpha") == -1, "Expected -1");
    PASS();

    TEST("shorter equal-prefix prerelease has lower precedence");
    ASSERT(updater_compare_versions("1.2.3-alpha", "1.2.3-alpha.1") == -1, "Expected -1");
    PASS();

    TEST("build metadata does not affect version precedence");
    ASSERT(updater_compare_versions("1.2.3+build.1", "1.2.3+build.2") == 0, "Expected 0");
    PASS();

    TEST("both NULL returns 0");
    ASSERT(updater_compare_versions(NULL, NULL) == 0, "Expected 0");
    PASS();

    TEST("v1 NULL returns 0");
    ASSERT(updater_compare_versions(NULL, "1.0.0") == 0, "Expected 0");
    PASS();
}

static void test_parse_full(void) {
    printf("\n--- updater_parse_response (full sample) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("Parse full JSON with EXE asset");
    ASSERT(updater_parse_response(SAMPLE_JSON_FULL, &info), "Expected parse success");
    PASS();

    TEST("tag_name is 'v2.1.0'");
    ASSERT(strcmp(info.tag_name, "v2.1.0") == 0, "Expected 'v2.1.0'");
    PASS();

    TEST("latest_version is '2.1.0' (v stripped)");
    ASSERT(strcmp(info.latest_version, "2.1.0") == 0, "Expected '2.1.0'");
    PASS();

    TEST("update_available is true");
    ASSERT(info.update_available == true, "Expected true");
    PASS();

    TEST("download_url contains .exe");
    ASSERT(ends_with(info.download_url, ".exe"), "Expected .exe URL");
    PASS();

    TEST("download_url contains expected path");
    ASSERT(strstr(info.download_url, "nosleep-2.1.0.exe") != NULL, "Expected nosleep-2.1.0.exe");
    PASS();
}

static void test_parse_no_exe(void) {
    printf("\n--- updater_parse_response (no EXE asset) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("ZIP-only release is unavailable and has no guessed download URL");
    ASSERT(!updater_parse_response(SAMPLE_JSON_NO_EXE, &info) &&
           !info.update_available && info.download_url[0] == '\0',
           "Expected no update without an EXE asset URL");
    PASS();
}

static void test_parse_exe_suffix(void) {
    printf("\n--- updater_parse_response (non-EXE suffix) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("asset URL ending in .exe.zip is unavailable");
    ASSERT(!parse_asset_url("https://github.com/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep-2.1.0.exe.zip", &info) &&
           !info.update_available && info.download_url[0] == '\0',
           "Expected only an .exe URL path to make an update available");
    PASS();
}

static void test_parse_malformed_exe_urls(void) {
    printf("\n--- updater_parse_response (malformed EXE URLs) ---\n");
    fflush(stdout);
    UpdateInfo info;
    static const char* invalid_urls[] = {
        "not-a-url.exe",
        "https:///nosleep-2.1.0.exe",
        "http://github.com/JohnXu22786/nosleep/nosleep-2.1.0.exe",
        "https://:443/nosleep-2.1.0.exe",
        "https://github.com:bogus/nosleep-2.1.0.exe",
        "https://github.com:65536/nosleep-2.1.0.exe"
    };

    TEST("malformed and non-HTTPS EXE URLs are unavailable");
    for (size_t i = 0; i < sizeof(invalid_urls) / sizeof(invalid_urls[0]); i++) {
        if (parse_asset_url(invalid_urls[i], &info) || info.update_available ||
            info.download_url[0] != '\0') {
            FAIL("Expected an absolute HTTPS URL with a valid host and port");
            return;
        }
    }
    PASS();
}

static void test_parse_exe_url_with_query(void) {
    printf("\n--- updater_parse_response (EXE URL with query) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("valid EXE path with a query remains available");
    ASSERT(parse_asset_url("https://github.com/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep-2.1.0.exe?download=1", &info) &&
           info.update_available &&
           strcmp(info.download_url,
                  "https://github.com/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep-2.1.0.exe?download=1") == 0,
           "Expected to preserve the asset URL including query text");
    PASS();
}

static void test_parse_exe_url_with_valid_port(void) {
    printf("\n--- updater_parse_response (EXE URL with valid port) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("valid HTTPS port preserves an executable asset URL");
    ASSERT(parse_asset_url("https://github.com:443/JohnXu22786/nosleep/nosleep-2.1.0.exe", &info) &&
           info.update_available &&
           strcmp(info.download_url,
                  "https://github.com:443/JohnXu22786/nosleep/nosleep-2.1.0.exe") == 0,
           "Expected valid HTTPS port to be accepted");
    PASS();
}

static void test_parse_no_tag(void) {
    printf("\n--- updater_parse_response (no tag_name) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("Parse JSON without tag_name");
    ASSERT(!updater_parse_response(SAMPLE_JSON_NO_TAG, &info), "Expected parse failure");
    PASS();
}

static void test_parse_no_exe_assets(void) {
    printf("\n--- updater_parse_response (no assets) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("release without an assets array is unavailable");
    ASSERT(!updater_parse_response(SAMPLE_JSON_NO_ASSETS, &info) &&
           !info.update_available && info.download_url[0] == '\0',
           "Expected no update without an EXE asset URL");
    PASS();
}

static void test_parse_empty_assets(void) {
    printf("\n--- updater_parse_response (empty assets) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("release with an empty assets array is unavailable");
    ASSERT(!updater_parse_response(SAMPLE_JSON_EMPTY_ASSETS, &info) &&
           !info.update_available && info.download_url[0] == '\0',
           "Expected no update without an EXE asset URL");
    PASS();
}

static void test_parse_no_leading_v(void) {
    printf("\n--- updater_parse_response (no leading v) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("Parse JSON with version without v prefix");
    ASSERT(updater_parse_response(SAMPLE_JSON_NO_V, &info), "Expected parse success");
    PASS();

    TEST("tag_name is '2.1.0'");
    ASSERT(strcmp(info.tag_name, "2.1.0") == 0, "Expected '2.1.0'");
    PASS();

    TEST("latest_version is '2.1.0' (no v to strip)");
    ASSERT(strcmp(info.latest_version, "2.1.0") == 0, "Expected '2.1.0'");
    PASS();

    TEST("download_url contains .exe");
    ASSERT(ends_with(info.download_url, ".exe"), "Expected .exe URL");
    PASS();
}

static void test_parse_invalid_json(void) {
    printf("\n--- updater_parse_response (invalid JSON) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("Parse invalid JSON returns false");
    ASSERT(!updater_parse_response(SAMPLE_JSON_INVALID, &info), "Expected parse failure");
    PASS();
}

static void test_parse_null_inputs(void) {
    printf("\n--- updater_parse_response (NULL inputs) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("NULL json_response returns false");
    ASSERT(!updater_parse_response(NULL, &info), "Expected false");
    PASS();

    TEST("NULL info returns false");
    ASSERT(!updater_parse_response(SAMPLE_JSON_FULL, NULL), "Expected false");
    PASS();
}

int main(void) {
    printf("========================================\n");
    printf("  updater module test suite\n");
    printf("========================================\n");
    fflush(stdout);

    test_ver_compare();
    test_parse_full();
    test_parse_no_exe();
    test_parse_exe_suffix();
    test_parse_malformed_exe_urls();
    test_parse_exe_url_with_query();
    test_parse_exe_url_with_valid_port();
    test_parse_no_tag();
    test_parse_no_exe_assets();
    test_parse_empty_assets();
    test_parse_no_leading_v();
    test_parse_invalid_json();
    test_parse_null_inputs();

    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");
    fflush(stdout);

    return tests_failed > 0 ? 1 : 0;
}
