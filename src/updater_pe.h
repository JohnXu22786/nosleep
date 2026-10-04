#ifndef NOSLEEP_UPDATER_PE_H
#define NOSLEEP_UPDATER_PE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*UpdaterPeReadAt)(void* context, uint64_t offset,
                                void* buffer, size_t length);

bool updater_pe_is_executable(uint64_t file_size, UpdaterPeReadAt read_at,
                              void* context);

#endif
