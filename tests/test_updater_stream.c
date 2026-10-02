// Tests the updater's response-stream copy logic with simulated read results.
#include "../src/updater_stream.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const char* data;
    size_t length;
    int calls;
    bool fail_after_data;
} FakeReader;

typedef struct {
    char data[32];
    size_t length;
} FakeWriter;

static bool fake_read(void* context, void* buffer, size_t capacity, size_t* bytes_read) {
    FakeReader* reader = (FakeReader*)context;
    if (reader->calls++ == 0) {
        if (reader->length > capacity) return false;
        memcpy(buffer, reader->data, reader->length);
        *bytes_read = reader->length;
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

static int test_read_error_after_partial_data(void) {
    static const char partial[] = "partial";
    FakeReader reader = {partial, sizeof(partial) - 1, 0, true};
    FakeWriter writer = {{0}, 0};
    char buffer[16];

    if (updater_copy_stream(fake_read, &reader, fake_write, &writer,
                            buffer, sizeof(buffer))) {
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
    FakeReader reader = {complete, sizeof(complete) - 1, 0, false};
    FakeWriter writer = {{0}, 0};
    char buffer[16];

    if (!updater_copy_stream(fake_read, &reader, fake_write, &writer,
                             buffer, sizeof(buffer))) {
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

int main(void) {
    int failures = 0;
    failures += test_read_error_after_partial_data();
    failures += test_clean_eof_after_data();
    printf("updater stream tests: %d passed, %d failed\n",
           2 - failures, failures);
    return failures == 0 ? 0 : 1;
}
