#ifndef NOSLEEP_UPDATER_TEMP_PATH_H
#define NOSLEEP_UPDATER_TEMP_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

static inline bool updater_build_temp_path(char* path, size_t path_capacity,
                                          const char* temp_dir,
                                          const char* unique_name) {
    if (!path || path_capacity == 0) return false;
    path[0] = '\0';
    if (!temp_dir || !unique_name) return false;

    int length = snprintf(path, path_capacity, "%s%s", temp_dir, unique_name);
    if (length < 0 || (size_t)length >= path_capacity) {
        path[0] = '\0';
        return false;
    }
    return true;
}

#endif
