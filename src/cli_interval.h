#ifndef CLI_INTERVAL_H
#define CLI_INTERVAL_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

static bool cli_parse_refresh_interval(const char *value, int *interval_seconds) {
    char *end = NULL;
    long parsed;

    if (value == NULL || interval_seconds == NULL) {
        return false;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (end == value || errno == ERANGE || parsed <= 0 || parsed > INT_MAX / 1000) {
        return false;
    }

    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }

    *interval_seconds = (int)parsed;
    return true;
}

#endif
