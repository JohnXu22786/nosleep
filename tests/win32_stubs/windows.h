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
typedef void *HMODULE;
typedef void (*FARPROC)(void);
typedef const wchar_t *LPCWSTR;

#define TRUE 1
#define FALSE 0
#define WINAPI
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_MORE_DATA 234
#define DELETE 0x00010000
#define REG_CREATED_NEW_KEY 1
#define KEY_QUERY_VALUE 0x0001
#define KEY_SET_VALUE 0x0002
#define KEY_CREATE_SUB_KEY 0x0004
#define KEY_ENUMERATE_SUB_KEYS 0x0008
#define KEY_NOTIFY 0x0010
#define READ_CONTROL 0x00020000
#define REG_OPTION_NON_VOLATILE 0
#define KEY_WRITE (READ_CONTROL | KEY_SET_VALUE | KEY_CREATE_SUB_KEY)
#define KEY_READ (READ_CONTROL | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY)
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
LONG RegCreateKeyExW(HKEY root, LPCWSTR path, DWORD reserved,
                     wchar_t *class_name, DWORD options, DWORD access,
                     void *security, HKEY *key, DWORD *disposition);
LONG RegOpenKeyExW(HKEY root, LPCWSTR path, DWORD reserved, DWORD access,
                   HKEY *key);
LONG RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type,
                    const BYTE *value, DWORD size);
LONG RegQueryValueExW(HKEY key, LPCWSTR name, DWORD *reserved, DWORD *type,
                      BYTE *value, DWORD *size);
LONG RegDeleteValueW(HKEY key, LPCWSTR name);
LONG RegCopyTreeW(HKEY source, LPCWSTR subkey, HKEY destination);
LONG RegDeleteTreeW(HKEY root, LPCWSTR path);
LONG RegDeleteKeyW(HKEY root, LPCWSTR path);
HMODULE GetModuleHandleW(LPCWSTR module_name);
FARPROC GetProcAddress(HMODULE module, const char *name);

#endif
