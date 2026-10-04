// Tests the updater's response-stream copy logic with simulated read results.
#include "../src/updater_stream.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const char* data;
    size_t length;
    size_t offset;
    int calls;
    bool fail_after_data;
    size_t max_chunk;
} FakeReader;

typedef struct {
    char data[32];
    size_t length;
} FakeWriter;

typedef struct {
    size_t values[8];
    size_t count;
    size_t cancel_after;
} FakeProgress;

static bool fake_read(void* context, void* buffer, size_t capacity, size_t* bytes_read) {
    FakeReader* reader = (FakeReader*)context;
    reader->calls++;
    if (reader->offset < reader->length) {
        size_t remaining = reader->length - reader->offset;
        size_t count = remaining < capacity ? remaining : capacity;
        if (reader->max_chunk > 0 && count > reader->max_chunk) {
            count = reader->max_chunk;
        }
        memcpy(buffer, reader->data + reader->offset, count);
        reader->offset += count;
        *bytes_read = count;
        return true;
    }

    *bytes_read = 0;
    return !reader->fail_after_data;
}

static bool fake_write(void* context, const void* buffer, size_t bytes_to_write,
                       size_t* bytes_written) {
    FakeWriter* writer = (FakeWriter*)context;
    if (bytes_to_write > sizeof(writer->data) - writer->length) return false;
    memcpy(writer->data + writer->length, buffer, bytes_to_write);
    writer->length += bytes_to_write;
    *bytes_written = bytes_to_write;
    return true;
}

static bool fake_progress(void* context, size_t bytes_copied) {
    FakeProgress* progress = (FakeProgress*)context;
    if (progress->count >= sizeof(progress->values) / sizeof(progress->values[0])) {
        return false;
    }
    progress->values[progress->count++] = bytes_copied;
    return progress->cancel_after == 0 || progress->count < progress->cancel_after;
}

static int test_read_error_after_partial_data(void) {
    static const char partial[] = "partial";
    FakeReader reader = {partial, sizeof(partial) - 1, 0, 0, true, 0};
    FakeWriter writer = {{0}, 0};
    char buffer[16];

    if (updater_copy_stream(fake_read, &reader, fake_write, &writer,
                            buffer, sizeof(buffer), NULL, NULL)) {
        fprintf(stderr, "read error after partial data was reported as success\n");
        return 1;
    }
    if (writer.length != sizeof(partial) - 1 ||
        memcmp(writer.data, partial, sizeof(partial) - 1) != 0) {
        fprintf(stderr, "expected the simulated read error after writing partial data\n");
        return 1;
    }
    return 0;
}

static int test_clean_eof_after_data(void) {
    static const char complete[] = "complete";
    FakeReader reader = {complete, sizeof(complete) - 1, 0, 0, false, 0};
    FakeWriter writer = {{0}, 0};
    char buffer[16];

    if (!updater_copy_stream(fake_read, &reader, fake_write, &writer,
                             buffer, sizeof(buffer), NULL, NULL)) {
        fprintf(stderr, "clean EOF after data was rejected\n");
        return 1;
    }
    if (writer.length != sizeof(complete) - 1 ||
        memcmp(writer.data, complete, sizeof(complete) - 1) != 0) {
        fprintf(stderr, "expected the full payload to be written\n");
        return 1;
    }
    return 0;
}

static int test_progress_reports_cumulative_bytes(void) {
    static const char content[] = "progress!";
    FakeReader reader = {content, sizeof(content) - 1, 0, 0, false, 4};
    FakeWriter writer = {{0}, 0};
    FakeProgress progress = {{0}, 0, 0};
    char buffer[8];

    if (!updater_copy_stream(fake_read, &reader, fake_write, &writer,
                             buffer, sizeof(buffer), fake_progress, &progress)) {
        fprintf(stderr, "stream with progress callback was rejected\n");
        return 1;
    }
    if (progress.count != 3 || progress.values[0] != 4 ||
        progress.values[1] != 8 || progress.values[2] != 9) {
        fprintf(stderr, "expected cumulative progress values 4, 8, and 9\n");
        return 1;
    }
    if (writer.length != sizeof(content) - 1 ||
        memcmp(writer.data, content, sizeof(content) - 1) != 0) {
        fprintf(stderr, "progress reporting changed the copied content\n");
        return 1;
    }
    return 0;
}

static int test_progress_callback_cancels_stream(void) {
    static const char content[] = "cancel-me";
    FakeReader reader = {content, sizeof(content) - 1, 0, 0, false, 3};
    FakeWriter writer = {{0}, 0};
    FakeProgress progress = {{0}, 0, 1};
    char buffer[8];

    if (updater_copy_stream(fake_read, &reader, fake_write, &writer,
                            buffer, sizeof(buffer), fake_progress, &progress)) {
        fprintf(stderr, "canceled stream was reported as success\n");
        return 1;
    }
    if (progress.count != 1 || progress.values[0] != 3 || reader.calls != 1 ||
        writer.length != 3 || memcmp(writer.data, content, 3) != 0) {
        fprintf(stderr, "cancel should stop before the next read and write\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_read_error_after_partial_data();
    failures += test_clean_eof_after_data();
    failures += test_progress_reports_cumulative_bytes();
    failures += test_progress_callback_cancels_stream();
    printf("updater stream tests: %d passed, %d failed\n",
           4 - failures, failures);
    return failures == 0 ? 0 : 1;
}
