#include "../src/updater_pe.h"

#include <stdio.h>
#include <string.h>

enum { PE_FIXTURE_SIZE = 1024, PE_HEADER_OFFSET = 0x80 };

typedef struct {
    unsigned char bytes[PE_FIXTURE_SIZE];
} FakePeFile;

static bool fake_read_at(void* context, uint64_t offset, void* buffer, size_t length) {
    FakePeFile* file = (FakePeFile*)context;
    if (offset > sizeof(file->bytes) || length > sizeof(file->bytes) - (size_t)offset) {
        return false;
    }
    memcpy(buffer, file->bytes + (size_t)offset, length);
    return true;
}

static void write_u16(unsigned char* destination, unsigned int value) {
    destination[0] = (unsigned char)(value & 0xFFu);
    destination[1] = (unsigned char)((value >> 8) & 0xFFu);
}

static void write_u32(unsigned char* destination, unsigned int value) {
    destination[0] = (unsigned char)(value & 0xFFu);
    destination[1] = (unsigned char)((value >> 8) & 0xFFu);
    destination[2] = (unsigned char)((value >> 16) & 0xFFu);
    destination[3] = (unsigned char)((value >> 24) & 0xFFu);
}

static void make_pe_fixture(FakePeFile* file, unsigned int optional_magic) {
    unsigned int optional_size = optional_magic == 0x020B ? 0xF0 : 0xE0;
    memset(file, 0, sizeof(*file));
    file->bytes[0] = 'M';
    file->bytes[1] = 'Z';
    write_u32(file->bytes + 0x3C, PE_HEADER_OFFSET);
    memcpy(file->bytes + PE_HEADER_OFFSET, "PE\0\0", 4);
    write_u16(file->bytes + PE_HEADER_OFFSET + 4,
              optional_magic == 0x020B ? 0x8664 : 0x014C);
    write_u16(file->bytes + PE_HEADER_OFFSET + 6, 1);
    write_u16(file->bytes + PE_HEADER_OFFSET + 20, optional_size);
    write_u16(file->bytes + PE_HEADER_OFFSET + 22, 0x0002);
    write_u16(file->bytes + PE_HEADER_OFFSET + 24, optional_magic);

    unsigned char* optional_header = file->bytes + PE_HEADER_OFFSET + 24;
    write_u32(optional_header + 4, 0x200);
    write_u32(optional_header + 16, 0x1000);
    write_u32(optional_header + 20, 0x1000);
    write_u32(optional_header + 32, 0x1000);
    write_u32(optional_header + 36, 0x200);
    write_u32(optional_header + 56, 0x2000);
    write_u32(optional_header + 60, 0x200);
    write_u16(optional_header + 68, 2);

    unsigned char* section = file->bytes + PE_HEADER_OFFSET + 24 + optional_size;
    write_u32(section + 8, 0x100);
    write_u32(section + 12, 0x1000);
    write_u32(section + 16, 0x200);
    write_u32(section + 20, 0x200);
    write_u32(section + 36, 0x60000020);
    file->bytes[0x200] = 0xC3;
}

static int test_rejects_html_response_body(void) {
    static const unsigned char html[] = "<html><body>Sign in to download</body></html>";
    FakePeFile file = {{0}};
    memcpy(file.bytes, html, sizeof(html) - 1);

    if (updater_pe_is_executable(sizeof(html) - 1, fake_read_at, &file)) {
        fprintf(stderr, "an HTML response body was accepted as an update executable\n");
        return 1;
    }
    return 0;
}

static int test_accepts_pe32_and_pe32_plus_headers(void) {
    static const unsigned int optional_magics[] = {0x010B, 0x020B};
    for (size_t i = 0; i < sizeof(optional_magics) / sizeof(optional_magics[0]); ++i) {
        FakePeFile file;
        make_pe_fixture(&file, optional_magics[i]);
        if (!updater_pe_is_executable(sizeof(file.bytes), fake_read_at, &file)) {
            fprintf(stderr, "a valid PE executable header was rejected (magic 0x%04X)\n",
                    optional_magics[i]);
            return 1;
        }
    }
    return 0;
}

static int test_rejects_truncated_or_malformed_headers(void) {
    FakePeFile file;
    make_pe_fixture(&file, 0x010B);
    if (updater_pe_is_executable(PE_HEADER_OFFSET + 8, fake_read_at, &file)) {
        fprintf(stderr, "a truncated PE header was accepted\n");
        return 1;
    }

    make_pe_fixture(&file, 0x010B);
    file.bytes[PE_HEADER_OFFSET] = 'N';
    if (updater_pe_is_executable(sizeof(file.bytes), fake_read_at, &file)) {
        fprintf(stderr, "a malformed PE signature was accepted\n");
        return 1;
    }

    make_pe_fixture(&file, 0x010B);
    write_u16(file.bytes + PE_HEADER_OFFSET + 22, 0x2002);
    if (updater_pe_is_executable(sizeof(file.bytes), fake_read_at, &file)) {
        fprintf(stderr, "a DLL was accepted as an executable update\n");
        return 1;
    }

    make_pe_fixture(&file, 0x010B);
    unsigned char* optional_header = file.bytes + PE_HEADER_OFFSET + 24;
    write_u32(optional_header + 56, 0);
    if (updater_pe_is_executable(sizeof(file.bytes), fake_read_at, &file)) {
        fprintf(stderr, "an image with zero SizeOfImage was accepted\n");
        return 1;
    }

    make_pe_fixture(&file, 0x010B);
    unsigned char* section = file.bytes + PE_HEADER_OFFSET + 24 + 0xE0;
    write_u32(section + 12, 0x1F00);
    if (updater_pe_is_executable(sizeof(file.bytes), fake_read_at, &file)) {
        fprintf(stderr, "a section extending beyond SizeOfImage was accepted\n");
        return 1;
    }

    make_pe_fixture(&file, 0x010B);
    write_u16(file.bytes + PE_HEADER_OFFSET + 4, 0);
    if (updater_pe_is_executable(sizeof(file.bytes), fake_read_at, &file)) {
        fprintf(stderr, "an executable with an unknown machine type was accepted\n");
        return 1;
    }

    make_pe_fixture(&file, 0x010B);
    write_u16(file.bytes + PE_HEADER_OFFSET + 4, 0x8664);
    if (updater_pe_is_executable(sizeof(file.bytes), fake_read_at, &file)) {
        fprintf(stderr, "a PE32 optional header with an x64 machine type was accepted\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_rejects_html_response_body();
    failures += test_accepts_pe32_and_pe32_plus_headers();
    failures += test_rejects_truncated_or_malformed_headers();
    printf("updater PE validation tests: %d passed, %d failed\n",
           3 - failures, failures);
    return failures == 0 ? 0 : 1;
}
