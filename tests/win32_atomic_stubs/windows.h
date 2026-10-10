#ifndef TEST_ATOMIC_WINDOWS_H
#define TEST_ATOMIC_WINDOWS_H

#include <stdbool.h>
#include <stdint.h>

typedef char CHAR;
typedef int32_t LONG;
typedef int BOOL;
typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
typedef unsigned int UINT;
typedef uintptr_t UINT_PTR;
typedef void *LPVOID;
typedef void *HANDLE;
typedef void *HICON;
typedef void *HWND;
typedef void *HMENU;
typedef struct { int unused; } NOTIFYICONDATA;
typedef struct { int unused; } CONDITION_VARIABLE;
typedef struct { int unused; } SRWLOCK;

#define WINAPI

static unsigned int test_interlocked_compare_exchange8_calls;
static unsigned int test_interlocked_compare_exchange_calls;

static inline CHAR InterlockedCompareExchange8(volatile CHAR *destination,
                                               CHAR exchange,
                                               CHAR comparand) {
    CHAR observed = comparand;
    ++test_interlocked_compare_exchange8_calls;
    __atomic_compare_exchange_n(destination, &observed, exchange, false,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return observed;
}

static inline LONG InterlockedCompareExchange(volatile LONG *destination,
                                              LONG exchange,
                                              LONG comparand) {
    LONG observed = comparand;
    ++test_interlocked_compare_exchange_calls;
    __atomic_compare_exchange_n(destination, &observed, exchange, false,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return observed;
}

#endif
