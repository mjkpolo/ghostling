#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <ghostty/vt.h>

#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

bool ghostling_decode_png(void *userdata,
                          const GhosttyAllocator *allocator,
                          const uint8_t *data,
                          size_t data_len,
                          GhosttySysImage *out)
{
    (void)userdata;
    if (data_len > INT_MAX) return false;

    int width, height, channels;
    stbi_uc *decoded = stbi_load_from_memory(
        data, (int)data_len, &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0) {
        stbi_image_free(decoded);
        return false;
    }

    size_t pixel_len = (size_t)width * (size_t)height * 4;
    uint8_t *pixels = ghostty_alloc(allocator, pixel_len);
    if (!pixels) {
        stbi_image_free(decoded);
        return false;
    }
    memcpy(pixels, decoded, pixel_len);
    stbi_image_free(decoded);

    out->width = (uint32_t)width;
    out->height = (uint32_t)height;
    out->data = pixels;
    out->data_len = pixel_len;
    return true;
}
