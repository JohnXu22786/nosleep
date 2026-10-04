#include "updater_pe.h"

#include <string.h>

enum {
    PE_DOS_HEADER_SIZE = 64,
    PE_SIGNATURE_AND_FILE_HEADER_SIZE = 24,
    PE_OPTIONAL_HEADER_PREFIX_SIZE = 60,
    PE_OPTIONAL_HEADER32_MIN_SIZE = 96,
    PE_OPTIONAL_HEADER64_MIN_SIZE = 112,
    PE_SECTION_HEADER_SIZE = 40,
    PE_MAX_SECTION_COUNT = 96,
    PE_FILE_MACHINE_I386 = 0x014C,
    PE_FILE_MACHINE_AMD64 = 0x8664,
    PE_FILE_EXECUTABLE_IMAGE = 0x0002,
    PE_FILE_DLL = 0x2000,
    PE_SECTION_CODE = 0x00000020,
    PE_SECTION_EXECUTE = 0x20000000
};

static unsigned int pe_read_u16(const unsigned char* bytes) {
    return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8);
}

static uint32_t pe_read_u32(const unsigned char* bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

bool updater_pe_is_executable(uint64_t file_size, UpdaterPeReadAt read_at,
                              void* context) {
    unsigned char dos_header[PE_DOS_HEADER_SIZE];
    if (!read_at || file_size < sizeof(dos_header) ||
        !read_at(context, 0, dos_header, sizeof(dos_header)) ||
        dos_header[0] != 'M' || dos_header[1] != 'Z') {
        return false;
    }

    uint32_t pe_offset = pe_read_u32(dos_header + 0x3C);
    if (pe_offset < sizeof(dos_header) || pe_offset > file_size ||
        file_size - pe_offset < PE_SIGNATURE_AND_FILE_HEADER_SIZE) {
        return false;
    }

    unsigned char pe_header[PE_SIGNATURE_AND_FILE_HEADER_SIZE];
    if (!read_at(context, pe_offset, pe_header, sizeof(pe_header)) ||
        memcmp(pe_header, "PE\0\0", 4) != 0) {
        return false;
    }

    unsigned int machine_type = pe_read_u16(pe_header + 4);
    unsigned int section_count = pe_read_u16(pe_header + 6);
    unsigned int optional_header_size = pe_read_u16(pe_header + 20);
    unsigned int characteristics = pe_read_u16(pe_header + 22);
    if (section_count == 0 || section_count > PE_MAX_SECTION_COUNT ||
        optional_header_size < PE_OPTIONAL_HEADER_PREFIX_SIZE ||
        !(characteristics & PE_FILE_EXECUTABLE_IMAGE) ||
        (characteristics & PE_FILE_DLL)) {
        return false;
    }

    uint64_t optional_header_offset =
        (uint64_t)pe_offset + PE_SIGNATURE_AND_FILE_HEADER_SIZE;
    if (optional_header_offset > file_size ||
        optional_header_size > file_size - optional_header_offset) {
        return false;
    }

    unsigned char optional_header_prefix[PE_OPTIONAL_HEADER_PREFIX_SIZE];
    if (!read_at(context, optional_header_offset, optional_header_prefix,
                 sizeof(optional_header_prefix))) {
        return false;
    }

    unsigned int optional_magic = pe_read_u16(optional_header_prefix);
    unsigned int minimum_optional_header_size;
    if (optional_magic == 0x010B) {
        if (machine_type != PE_FILE_MACHINE_I386) return false;
        minimum_optional_header_size = PE_OPTIONAL_HEADER32_MIN_SIZE;
    } else if (optional_magic == 0x020B) {
        if (machine_type != PE_FILE_MACHINE_AMD64) return false;
        minimum_optional_header_size = PE_OPTIONAL_HEADER64_MIN_SIZE;
    } else {
        return false;
    }
    if (optional_header_size < minimum_optional_header_size) return false;

    uint32_t entry_point = pe_read_u32(optional_header_prefix + 16);
    uint32_t size_of_image = pe_read_u32(optional_header_prefix + 56);
    if (entry_point == 0 || size_of_image == 0 || entry_point >= size_of_image) {
        return false;
    }

    uint64_t section_table_offset = optional_header_offset + optional_header_size;
    uint64_t section_table_size = (uint64_t)section_count * PE_SECTION_HEADER_SIZE;
    if (section_table_offset > file_size ||
        section_table_size > file_size - section_table_offset) {
        return false;
    }

    bool entry_point_in_code = false;
    for (unsigned int i = 0; i < section_count; ++i) {
        unsigned char section[PE_SECTION_HEADER_SIZE];
        uint64_t section_offset =
            section_table_offset + (uint64_t)i * PE_SECTION_HEADER_SIZE;
        if (!read_at(context, section_offset, section, sizeof(section))) return false;

        uint32_t virtual_size = pe_read_u32(section + 8);
        uint32_t virtual_address = pe_read_u32(section + 12);
        uint32_t raw_size = pe_read_u32(section + 16);
        uint32_t raw_offset = pe_read_u32(section + 20);
        uint32_t section_characteristics = pe_read_u32(section + 36);
        uint32_t image_section_size = virtual_size > raw_size ? virtual_size : raw_size;
        if ((uint64_t)virtual_address > size_of_image ||
            (uint64_t)image_section_size > size_of_image - virtual_address) {
            return false;
        }
        if (raw_size > 0 &&
            ((uint64_t)raw_offset > file_size ||
             (uint64_t)raw_size > file_size - raw_offset)) {
            return false;
        }

        if (raw_size > 0 && entry_point >= virtual_address &&
            (uint64_t)entry_point - virtual_address < raw_size &&
            (section_characteristics & (PE_SECTION_CODE | PE_SECTION_EXECUTE)) ==
                (PE_SECTION_CODE | PE_SECTION_EXECUTE)) {
            entry_point_in_code = true;
        }
    }

    return entry_point_in_code;
}
