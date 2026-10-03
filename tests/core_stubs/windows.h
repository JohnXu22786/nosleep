#ifndef TEST_CORE_WINDOWS_H
#define TEST_CORE_WINDOWS_H

#include <stdarg.h>
#include <stdint.h>

typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
typedef int BOOL;
typedef struct TestEvent {
    BOOL signaled;
} *HANDLE;
typedef struct {
    unsigned short wHour;
    unsigned short wMinute;
    unsigned short wSecond;
} SYSTEMTIME;
typedef struct {
    int unused;
} SRWLOCK;

#define TRUE 1
#define FALSE 0
#define WINAPI
#define SRWLOCK_INIT {0}
#define ES_CONTINUOUS 0x80000000u
#define ES_SYSTEM_REQUIRED 0x00000001u
#define ES_DISPLAY_REQUIRED 0x00000002u
#define ES_AWAYMODE_REQUIRED 0x00000040u
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u

HANDLE CreateEvent(void *attributes, BOOL manual_reset, BOOL initial_state,
                   const char *name);
BOOL CloseHandle(HANDLE event);
BOOL ResetEvent(HANDLE event);
BOOL SetEvent(HANDLE event);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *events, BOOL wait_all,
                             DWORD timeout);
ULONGLONG GetTickCount64(void);
DWORD SetThreadExecutionState(DWORD flags);
void Sleep(DWORD milliseconds);
void GetSystemTime(SYSTEMTIME *time);
void AcquireSRWLockExclusive(SRWLOCK *lock);
void ReleaseSRWLockExclusive(SRWLOCK *lock);

#endif
