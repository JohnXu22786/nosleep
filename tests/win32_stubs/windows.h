#ifndef TEST_WINDOWS_H
#define TEST_WINDOWS_H

#include <stddef.h>
#include <stdint.h>

typedef struct TestIcon *HICON;
typedef void *HWND;
typedef int BOOL;
typedef unsigned int DWORD;
typedef long LONG;
typedef unsigned char BYTE;
typedef BYTE *LPBYTE;
typedef void *HKEY;

#define TRUE 1
#define FALSE 0
#define WINAPI
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_MORE_DATA 234
#define REG_OPTION_NON_VOLATILE 0
#define KEY_WRITE 0
#define KEY_READ 0
#define REG_SZ 1
#define REG_DWORD 4
#define HKEY_CURRENT_USER ((HKEY)(uintptr_t)1)

LONG RegCreateKeyEx(HKEY root, const char *path, DWORD reserved,
                    const char *class_name, DWORD options, DWORD access,
                    void *security, HKEY *key, DWORD *disposition);
LONG RegOpenKeyEx(HKEY root, const char *path, DWORD reserved, DWORD access,
                  HKEY *key);
LONG RegSetValueEx(HKEY key, const char *name, DWORD reserved, DWORD type,
                   const BYTE *value, DWORD size);
LONG RegQueryValueEx(HKEY key, const char *name, DWORD *reserved, DWORD *type,
                     BYTE *value, DWORD *size);
LONG RegDeleteKey(HKEY root, const char *path);
LONG RegCloseKey(HKEY key);
LONG RegRenameKey(HKEY key, const wchar_t *subkey_name, const wchar_t *new_name);

#endif
