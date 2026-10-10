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
    "  \"html_url\": \"https://github.com/JohnXu22786/nosleep/releases/tag/v2.1.0\","
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

static const char* SAMPLE_JSON_MALFORMED_ASSETS[] = {
    "{\"tag_name\":\"v2.1.0\",\"assets\":[null]}",
    "{\"tag_name\":\"v2.1.0\",\"assets\":[{\"name\":\"nosleep.zip\"}]}",
    "{\"tag_name\":\"v2.1.0\",\"assets\":[{\"browser_download_url\":\"\"}]}"
};

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

    TEST("multi-digit core components cannot start with zero");
    ASSERT(updater_compare_versions("01.2.3", "1.2.3") == -2 &&
           updater_compare_versions("1.02.3", "1.2.3") == -2 &&
           updater_compare_versions("1.2.03", "1.2.3") == -2,
           "Expected invalid-version result");
    PASS();

    TEST("single zero core and prerelease identifiers remain valid");
    ASSERT(updater_compare_versions("0.0.0-0", "0.0.0") == -1,
           "Expected a valid prerelease version");
    PASS();

    TEST("multi-digit numeric prerelease identifiers cannot start with zero");
    ASSERT(updater_compare_versions("1.2.3-001", "1.2.3-10") == -2 &&
           updater_compare_versions("1.2.3-rc.01", "1.2.3-rc.1") == -2,
           "Expected invalid-version result");
    PASS();

    TEST("build metadata permits numeric identifiers with leading zeros");
    ASSERT(updater_compare_versions("1.2.3+001", "1.2.3") == 0 &&
           updater_compare_versions("1.2.3-rc.1+001", "1.2.3-rc.1") == 0,
           "Build metadata must not affect precedence or version validity");
    PASS();

    TEST("shorter equal-prefix prerelease has lower precedence");
    ASSERT(updater_compare_versions("1.2.3-alpha", "1.2.3-alpha.1") == -1, "Expected -1");
    PASS();

    TEST("build metadata does not affect version precedence");
    ASSERT(updater_compare_versions("1.2.3+build.1", "1.2.3+build.2") == 0, "Expected 0");
    PASS();

    TEST("malformed version suffix is not treated as an equal numeric core");
    ASSERT(updater_compare_versions("v1.2.3garbage", "1.2.3") == -2,
           "Expected invalid-version result");
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

static void test_release_page_url(void) {
    printf("\n--- updater_get_release_page_url ---\n");
    fflush(stdout);
    UpdateInfo info = {0};
    static const char* fallback =
        "https://github.com/JohnXu22786/nosleep/releases";
    static const char* release_page =
        "https://github.com/JohnXu22786/nosleep/releases/tag/v2.1.0";

    strcpy(info.release_notes_url, release_page);
    strcpy(info.download_url,
           "https://attacker.example/nosleep.exe");
    TEST("use a valid release page and never fall back to the download URL");
    ASSERT(strcmp(updater_get_release_page_url(&info), release_page) == 0,
           "Expected validated release page URL");
    PASS();

    info.release_notes_url[0] = '\0';
    TEST("use official releases listing when release notes are absent");
    ASSERT(strcmp(updater_get_release_page_url(&info), fallback) == 0,
           "Expected official releases listing");
    PASS();

    strcpy(info.release_notes_url, "https://attacker.example/releases");
    TEST("use official releases listing when the release page is unvalidated");
    ASSERT(strcmp(updater_get_release_page_url(&info), fallback) == 0,
           "Expected malformed release page to be rejected");
    PASS();
}

static void test_parse_no_exe(void) {
    printf("\n--- updater_parse_response (no EXE asset) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("ZIP-only release parses successfully without an installable asset");
    ASSERT(updater_parse_response(SAMPLE_JSON_NO_EXE, &info) &&
           !info.update_available && info.download_url[0] == '\0',
           "Expected release check success without an EXE asset URL");
    ASSERT(strcmp(info.latest_version, "2.1.0") == 0 &&
           strcmp(updater_get_release_page_url(&info),
                  "https://github.com/JohnXu22786/nosleep/releases/tag/v2.1.0") == 0,
           "Expected release version and official page to remain available");
    PASS();
}

static void test_parse_exe_suffix(void) {
    printf("\n--- updater_parse_response (non-EXE suffix) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("asset URL ending in .exe.zip is not installable");
    ASSERT(parse_asset_url("https://github.com/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep-2.1.0.exe.zip", &info) &&
           !info.update_available && info.download_url[0] == '\0',
           "Expected valid release lookup without an installable .exe");
    PASS();
}

static void test_parse_exe_suffix_case_insensitive(void) {
    printf("\n--- updater_parse_response (case-insensitive EXE suffix) ---\n");
    fflush(stdout);
    UpdateInfo info;
    static const char* asset_urls[] = {
        "https://github.com/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep-2.1.0.EXE",
        "https://github.com/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep-2.1.0.Exe"
    };

    TEST("uppercase and mixed-case EXE suffixes remain installable");
    for (size_t i = 0; i < sizeof(asset_urls) / sizeof(asset_urls[0]); i++) {
        if (!parse_asset_url(asset_urls[i], &info) || !info.update_available ||
            strcmp(info.download_url, asset_urls[i]) != 0) {
            FAIL("Expected mixed-case executable suffix to preserve the installable asset URL");
            return;
        }
    }
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

    TEST("malformed and non-HTTPS EXE URLs do not prevent a valid release check");
    for (size_t i = 0; i < sizeof(invalid_urls) / sizeof(invalid_urls[0]); i++) {
        if (!parse_asset_url(invalid_urls[i], &info) || info.update_available ||
            info.download_url[0] != '\0') {
            FAIL("Expected valid release without a usable EXE asset");
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

static void test_parse_official_release_asset_url(void) {
    printf("\n--- updater_parse_response (official release asset URL) ---\n");
    fflush(stdout);
    UpdateInfo info;
    static const char* official_url =
        "https://github.com/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep-2.1.0.exe?download=1";

    TEST("official repository release asset remains installable with its query");
    ASSERT(parse_asset_url(official_url, &info) && info.update_available &&
           strcmp(info.download_url, official_url) == 0,
           "Expected the official release asset URL to be preserved");
    PASS();
}

static void test_parse_nonofficial_release_asset_urls(void) {
    printf("\n--- updater_parse_response (nonofficial release asset URLs) ---\n");
    fflush(stdout);
    UpdateInfo info;
    static const char* invalid_urls[] = {
        "https://attacker.example/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep.exe",
        "https://github.com.attacker.example/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep.exe",
        "https://github.com:8443/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep.exe",
        "https://github.com/attacker/nosleep/releases/download/v2.1.0/nosleep.exe",
        "https://github.com/JohnXu22786/other/releases/download/v2.1.0/nosleep.exe",
        "https://github.com/JohnXu22786/nosleep-evil/releases/download/v2.1.0/nosleep.exe",
        "https://github.com/JohnXu22786evil/nosleep/releases/download/v2.1.0/nosleep.exe",
        "https://github.com/JohnXu22786/nosleep/releases/download-evil/v2.1.0/nosleep.exe"
    };

    TEST("unrelated hosts, repositories, and prefix-confusion paths are not installable");
    for (size_t i = 0; i < sizeof(invalid_urls) / sizeof(invalid_urls[0]); i++) {
        if (!parse_asset_url(invalid_urls[i], &info) || info.update_available ||
            info.download_url[0] != '\0') {
            FAIL("Expected nonofficial EXE URL to be ignored");
            return;
        }
    }
    PASS();
}

static void test_parse_exe_url_at_buffer_limit(void) {
    printf("\n--- updater_parse_response (EXE URL buffer limit) ---\n");
    fflush(stdout);
    UpdateInfo info;
    static const char prefix[] =
        "https://github.com/JohnXu22786/nosleep/releases/download/v2.1.0/";
    static const char suffix[] = ".exe";
    char url[sizeof(info.download_url) + 1];
    const size_t prefix_len = sizeof(prefix) - 1;
    const size_t suffix_len = sizeof(suffix) - 1;

    TEST("511-byte installer URL fits the 512-byte download buffer");
    size_t url_len = sizeof(info.download_url) - 1;
    memcpy(url, prefix, prefix_len);
    memset(url + prefix_len, 'a', url_len - prefix_len - suffix_len);
    memcpy(url + url_len - suffix_len, suffix, suffix_len);
    url[url_len] = '\0';
    ASSERT(parse_asset_url(url, &info) && info.update_available &&
           strlen(info.download_url) == url_len &&
           strcmp(info.download_url, url) == 0,
           "Expected 511-byte installer URL to be preserved");
    PASS();

    TEST("512-byte installer URL remains too large for the download buffer");
    url_len = sizeof(info.download_url);
    memcpy(url, prefix, prefix_len);
    memset(url + prefix_len, 'a', url_len - prefix_len - suffix_len);
    memcpy(url + url_len - suffix_len, suffix, suffix_len);
    url[url_len] = '\0';
    ASSERT(parse_asset_url(url, &info) && !info.update_available &&
           info.download_url[0] == '\0',
           "Expected over-capacity installer URL to be ignored");
    PASS();
}

static void test_parse_exe_url_with_valid_port(void) {
    printf("\n--- updater_parse_response (EXE URL with valid port) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("valid HTTPS port preserves an executable asset URL");
    ASSERT(parse_asset_url("https://github.com:443/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep-2.1.0.exe", &info) &&
           info.update_available &&
           strcmp(info.download_url,
                  "https://github.com:443/JohnXu22786/nosleep/releases/download/v2.1.0/nosleep-2.1.0.exe") == 0,
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

static void test_parse_malformed_tag(void) {
    printf("\n--- updater_parse_response (malformed release tag) ---\n");
    fflush(stdout);
    UpdateInfo info;
    const char* malformed_release =
        "{\"tag_name\":\"v1.2.3garbage\",\"assets\":["
        "{\"browser_download_url\":"
        "\"https://github.com/JohnXu22786/nosleep/releases/download/v1.2.3/nosleep-1.2.3.exe\"}]}";

    TEST("release tag with trailing garbage is rejected");
    ASSERT(!updater_parse_response(malformed_release, &info) &&
           !info.update_available && info.download_url[0] == '\0',
           "Expected malformed release tag to be unavailable");
    PASS();
}

static void test_parse_no_exe_assets(void) {
    printf("\n--- updater_parse_response (no assets) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("release missing the required assets array remains a parse failure");
    ASSERT(!updater_parse_response(SAMPLE_JSON_NO_ASSETS, &info) &&
           !info.update_available && info.download_url[0] == '\0',
           "Expected response missing assets to remain malformed");
    PASS();
}

static void test_parse_empty_assets(void) {
    printf("\n--- updater_parse_response (empty assets) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("release with an empty assets array is a successful check without an installer");
    ASSERT(updater_parse_response(SAMPLE_JSON_EMPTY_ASSETS, &info) &&
           !info.update_available && info.download_url[0] == '\0',
           "Expected successful release check without an EXE asset URL");
    PASS();
}

static void test_parse_malformed_assets(void) {
    printf("\n--- updater_parse_response (malformed assets) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("release assets with a non-object item, missing URL, or empty URL remain parse failures");
    for (size_t i = 0; i < sizeof(SAMPLE_JSON_MALFORMED_ASSETS) /
                            sizeof(SAMPLE_JSON_MALFORMED_ASSETS[0]); i++) {
        if (updater_parse_response(SAMPLE_JSON_MALFORMED_ASSETS[i], &info) ||
            info.update_available || info.download_url[0] != '\0') {
            FAIL("Expected malformed asset entries to remain a parse failure");
            return;
        }
    }
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
    test_release_page_url();
    test_parse_no_exe();
    test_parse_exe_suffix();
    test_parse_exe_suffix_case_insensitive();
    test_parse_malformed_exe_urls();
    test_parse_exe_url_with_query();
    test_parse_official_release_asset_url();
    test_parse_nonofficial_release_asset_urls();
    test_parse_exe_url_at_buffer_limit();
    test_parse_exe_url_with_valid_port();
    test_parse_no_tag();
    test_parse_malformed_tag();
    test_parse_no_exe_assets();
    test_parse_empty_assets();
    test_parse_malformed_assets();
    test_parse_no_leading_v();
    test_parse_invalid_json();
    test_parse_null_inputs();

    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");
    fflush(stdout);

    return tests_failed > 0 ? 1 : 0;
}
