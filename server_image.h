#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <ghostty/vt.h>

bool ghostling_decode_png(void *userdata,
                          const GhosttyAllocator *allocator,
                          const uint8_t *data,
                          size_t data_len,
                          GhosttySysImage *out);
