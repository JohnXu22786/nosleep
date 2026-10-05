#ifndef NOSLEEP_UPDATER_URL_POLICY_H
#define NOSLEEP_UPDATER_URL_POLICY_H

#include <stdbool.h>
#include <wchar.h>

static inline bool updater_url_is_https(const wchar_t* url) {
    static const wchar_t https_scheme[] = L"https://";
    size_t i;

    if (!url) return false;

    for (i = 0; https_scheme[i] != L'\0'; i++) {
        wchar_t character = url[i];
        if (character >= L'A' && character <= L'Z') {
            character = (wchar_t)(character - L'A' + L'a');
        }
        if (character != https_scheme[i]) return false;
    }

    return url[i] != L'\0';
}

#endif
