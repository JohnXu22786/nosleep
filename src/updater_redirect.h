#ifndef NOSLEEP_UPDATER_REDIRECT_H
#define NOSLEEP_UPDATER_REDIRECT_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

static inline bool updater_build_request_target(const wchar_t* path,
                                                const wchar_t* extra_info,
                                                wchar_t* target,
                                                size_t target_capacity) {
    if (!path || !extra_info || !target || target_capacity == 0) return false;

    size_t path_length = wcslen(path);
    const wchar_t* fragment = wcschr(extra_info, L'#');
    size_t extra_info_length = fragment
        ? (size_t)(fragment - extra_info)
        : wcslen(extra_info);
    if (path_length >= target_capacity ||
        extra_info_length >= target_capacity - path_length) {
        return false;
    }

    wmemcpy(target, path, path_length);
    wmemcpy(target + path_length, extra_info, extra_info_length);
    target[path_length + extra_info_length] = L'\0';
    return true;
}

#endif
