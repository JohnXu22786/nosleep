#define _WIN32_WINNT 0x0600
#include <windows.h>

#include <stdio.h>

#include "core.h"

#define THREAD_COUNT 8
#define LOGS_PER_THREAD 30

typedef struct {
    int thread_id;
    int logs_done;
} ThreadData;

static DWORD WINAPI log_thread(LPVOID parameter) {
    ThreadData *data = (ThreadData *)parameter;
    for (int i = 0; i < LOGS_PER_THREAD; ++i) {
        nosleep_log_info("Thread %d iteration %d", data->thread_id, i);
        nosleep_log_warning("Thread %d warning %d", data->thread_id, i);
        nosleep_log_error("Thread %d error %d", data->thread_id, i);
        ++data->logs_done;

        if (i % 5 == 0) {
            Sleep(0);
        }
    }
    return 0;
}

int main(void) {
    HANDLE threads[THREAD_COUNT];
    ThreadData thread_data[THREAD_COUNT] = {{0}};
    DWORD created_threads = 0;
    int failed = 0;

    nosleep_log_info("Test info message");
    nosleep_log_warning("Test warning message");
    nosleep_log_error("Test error message");

    for (int i = 0; i < THREAD_COUNT; ++i) {
        thread_data[i].thread_id = i;
        threads[i] = CreateThread(NULL, 0, log_thread, &thread_data[i], 0, NULL);
        if (threads[i] == NULL) {
            fprintf(stderr, "FAIL: Failed to create thread %d\n", i);
            failed = 1;
            break;
        }
        ++created_threads;
    }

    if (created_threads > 0) {
        DWORD wait_result = WaitForMultipleObjects(created_threads, threads, TRUE, 30000);
        if (wait_result != WAIT_OBJECT_0) {
            fprintf(stderr, "FAIL: Threads did not complete (wait result %lu)\n",
                    (unsigned long)wait_result);
            failed = 1;
        }
    }

    int completed_iterations = 0;
    for (DWORD i = 0; i < created_threads; ++i) {
        if (thread_data[i].logs_done != LOGS_PER_THREAD) {
            fprintf(stderr, "FAIL: Thread %lu completed %d iterations\n",
                    (unsigned long)i, thread_data[i].logs_done);
            failed = 1;
        }
        completed_iterations += thread_data[i].logs_done;
        CloseHandle(threads[i]);
    }

    if (created_threads != THREAD_COUNT ||
        completed_iterations != THREAD_COUNT * LOGS_PER_THREAD) {
        fprintf(stderr, "FAIL: Expected %d threads and %d iterations, got %lu and %d\n",
                THREAD_COUNT, THREAD_COUNT * LOGS_PER_THREAD,
                (unsigned long)created_threads, completed_iterations);
        failed = 1;
    }

    if (!failed) {
        puts("PASS: native Windows thread logging completed without failure");
    }
    return failed;
}
