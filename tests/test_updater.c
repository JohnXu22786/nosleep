// Test suite for updater module
// Tests JSON parsing and version comparison functions
// Standalone test - does not link updater.c (avoids WinHTTP dependency)
#include "updater.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  TEST: %s ... ", name); fflush(stdout); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; fflush(stdout); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; fflush(stdout); } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// ---- Helper functions (inline copies from updater.c) ----

static int ver_compare(const char* v1, const char* v2) {
    if (!v1 || !v2) return 0;
    if (v1[0] == 'v' || v1[0] == 'V') v1++;
    if (v2[0] == 'v' || v2[0] == 'V') v2++;
    int maj1 = 0, min1 = 0, pat1 = 0;
    int maj2 = 0, min2 = 0, pat2 = 0;
    sscanf(v1, "%d.%d.%d", &maj1, &min1, &pat1);
    sscanf(v2, "%d.%d.%d", &maj2, &min2, &pat2);
    if (maj1 > maj2) return 1;
    if (maj1 < maj2) return -1;
    if (min1 > min2) return 1;
    if (min1 < min2) return -1;
    if (pat1 > pat2) return 1;
    if (pat1 < pat2) return -1;
    return 0;
}

static bool parse_json_response(const char* json_response, UpdateInfo* info) {
    if (!json_response || !info) return false;
    memset(info, 0, sizeof(UpdateInfo));
    cJSON* root = cJSON_Parse(json_response);
    if (!root) return false;
    cJSON* tag_name = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    if (!cJSON_IsString(tag_name) || !tag_name->valuestring) {
        cJSON_Delete(root);
        return false;
    }
    size_t len = strlen(tag_name->valuestring);
    if (len == 0 || len >= sizeof(info->tag_name)) {
        cJSON_Delete(root);
        return false;
    }
    strncpy(info->tag_name, tag_name->valuestring, sizeof(info->tag_name) - 1);
    info->tag_name[sizeof(info->tag_name) - 1] = '\0';
    const char* ver = info->tag_name;
    if (ver[0] == 'v' || ver[0] == 'V') ver++;
    strncpy(info->latest_version, ver, sizeof(info->latest_version) - 1);
    info->latest_version[sizeof(info->latest_version) - 1] = '\0';
    cJSON* assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    if (cJSON_IsArray(assets)) {
        cJSON* asset = NULL;
        cJSON_ArrayForEach(asset, assets) {
            cJSON* url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
            if (cJSON_IsString(url) && url->valuestring) {
                size_t url_len = strlen(url->valuestring);
                if (url_len > 0 && url_len < sizeof(info->download_url) - 1) {
                    strncpy(info->download_url, url->valuestring, sizeof(info->download_url) - 1);
                    info->download_url[sizeof(info->download_url) - 1] = '\0';
                    if (strstr(info->download_url, ".exe") != NULL) {
                        info->update_available = true;
                        cJSON_Delete(root);
                        return true;
                    }
                }
            }
        }
    }
    if (info->tag_name[0] != '\0') {
        info->update_available = true;
        snprintf(info->download_url, sizeof(info->download_url),
            "https://github.com/JohnXu22786/nosleep/releases/download/%s/nosleep-%s.exe",
            info->tag_name, info->tag_name);
        cJSON_Delete(root);
        return true;
    }
    cJSON_Delete(root);
    return false;
}

static int ends_with(const char* str, const char* suffix) {
    if (!str || !suffix) return 0;
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len) return 0;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
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
    ASSERT(ver_compare("2.0.0", "1.9.9") == 1, "Expected 1");
    PASS();

    TEST("v1 < v2 (major)");
    ASSERT(ver_compare("1.9.9", "2.0.0") == -1, "Expected -1");
    PASS();

    TEST("v1 > v2 (minor)");
    ASSERT(ver_compare("1.2.0", "1.1.9") == 1, "Expected 1");
    PASS();

    TEST("v1 < v2 (minor)");
    ASSERT(ver_compare("1.1.9", "1.2.0") == -1, "Expected -1");
    PASS();

    TEST("v1 > v2 (patch)");
    ASSERT(ver_compare("1.1.2", "1.1.1") == 1, "Expected 1");
    PASS();

    TEST("v1 == v2");
    ASSERT(ver_compare("1.1.1", "1.1.1") == 0, "Expected 0");
    PASS();

    TEST("v1 == v2 with leading v");
    ASSERT(ver_compare("v1.1.1", "1.1.1") == 0, "Expected 0");
    PASS();

    TEST("v1 == v2 with leading V");
    ASSERT(ver_compare("V1.1.1", "1.1.1") == 0, "Expected 0");
    PASS();

    TEST("both NULL returns 0");
    ASSERT(ver_compare(NULL, NULL) == 0, "Expected 0");
    PASS();

    TEST("v1 NULL returns 0");
    ASSERT(ver_compare(NULL, "1.0.0") == 0, "Expected 0");
    PASS();
}

static void test_parse_full(void) {
    printf("\n--- updater_parse_response (full sample) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("Parse full JSON with EXE asset");
    ASSERT(parse_json_response(SAMPLE_JSON_FULL, &info), "Expected parse success");
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

    TEST("Parse JSON without EXE asset");
    ASSERT(parse_json_response(SAMPLE_JSON_NO_EXE, &info), "Expected parse success (fallback)");
    PASS();

    TEST("tag_name is 'v2.1.0'");
    ASSERT(strcmp(info.tag_name, "v2.1.0") == 0, "Expected 'v2.1.0'");
    PASS();

    TEST("update_available is true");
    ASSERT(info.update_available == true, "Expected true");
    PASS();

    TEST("download_url is fallback URL");
    ASSERT(strstr(info.download_url, "releases/download") != NULL, "Expected fallback URL");
    PASS();
}

static void test_parse_no_tag(void) {
    printf("\n--- updater_parse_response (no tag_name) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("Parse JSON without tag_name");
    ASSERT(!parse_json_response(SAMPLE_JSON_NO_TAG, &info), "Expected parse failure");
    PASS();
}

static void test_parse_no_exe_assets(void) {
    printf("\n--- updater_parse_response (no assets) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("Parse JSON with no assets array");
    ASSERT(parse_json_response(SAMPLE_JSON_NO_ASSETS, &info), "Expected parse success (fallback)");
    PASS();

    TEST("tag_name is 'v2.1.0'");
    ASSERT(strcmp(info.tag_name, "v2.1.0") == 0, "Expected 'v2.1.0'");
    PASS();

    TEST("download_url is fallback");
    ASSERT(strlen(info.download_url) > 0, "Expected non-empty fallback URL");
    PASS();
}

static void test_parse_empty_assets(void) {
    printf("\n--- updater_parse_response (empty assets) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("Parse JSON with empty assets array");
    ASSERT(parse_json_response(SAMPLE_JSON_EMPTY_ASSETS, &info), "Expected parse success (fallback)");
    PASS();

    TEST("download_url is fallback");
    ASSERT(strlen(info.download_url) > 0, "Expected non-empty fallback URL");
    PASS();
}

static void test_parse_no_leading_v(void) {
    printf("\n--- updater_parse_response (no leading v) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("Parse JSON with version without v prefix");
    ASSERT(parse_json_response(SAMPLE_JSON_NO_V, &info), "Expected parse success");
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
    ASSERT(!parse_json_response(SAMPLE_JSON_INVALID, &info), "Expected parse failure");
    PASS();
}

static void test_parse_null_inputs(void) {
    printf("\n--- updater_parse_response (NULL inputs) ---\n");
    fflush(stdout);
    UpdateInfo info;

    TEST("NULL json_response returns false");
    ASSERT(!parse_json_response(NULL, &info), "Expected false");
    PASS();

    TEST("NULL info returns false");
    ASSERT(!parse_json_response(SAMPLE_JSON_FULL, NULL), "Expected false");
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
