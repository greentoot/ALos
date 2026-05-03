#ifndef KERNEL_JACK_JACK_ASSETS_H
#define KERNEL_JACK_JACK_ASSETS_H

#include <stdint.h>

/* Optional binary assets embedded via linker objects (ROMs, covers, etc.). */
void jack_assets_mount(void);
int jack_assets_get_generated_cover_preview(const char *title, const uint8_t **out_pixels, int *out_w, int *out_h);
int jack_assets_get_cover_preview(const char *title, const uint8_t **out_pixels, int *out_w, int *out_h);

#endif
