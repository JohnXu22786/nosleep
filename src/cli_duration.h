#ifndef CLI_DURATION_H
#define CLI_DURATION_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

static bool cli_parse_duration(const char *value, int *duration) {
    char *end = NULL;
    long parsed;

    if (value == NULL || duration == NULL) {
        return false;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (end == value || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }

    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }

    *duration = (int)parsed;
    return true;
}

#endif
