#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBA GBA;

#define GBA_WIDTH 240
#define GBA_HEIGHT 160
#define GBA_FRAMEBUFFER_PIXELS (GBA_WIDTH * GBA_HEIGHT)

enum {
    GBA_KEY_A = 1 << 0,
    GBA_KEY_B = 1 << 1,
    GBA_KEY_SELECT = 1 << 2,
    GBA_KEY_START = 1 << 3,
    GBA_KEY_RIGHT = 1 << 4,
    GBA_KEY_LEFT = 1 << 5,
    GBA_KEY_UP = 1 << 6,
    GBA_KEY_DOWN = 1 << 7,
    GBA_KEY_R = 1 << 8,
    GBA_KEY_L = 1 << 9
};

GBA *gba_create(void);
void gba_destroy(GBA *gba);
void gba_reset(GBA *gba);
bool gba_load_rom(GBA *gba, const uint8_t *data, size_t size);
void gba_load_demo(GBA *gba);

void gba_run_frame(GBA *gba);
const uint32_t *gba_framebuffer(const GBA *gba);
void gba_set_keys(GBA *gba, uint16_t keys_pressed);
int gba_audio_samples(GBA *gba, float *out, int max_frames);

const char *gba_rom_title(const GBA *gba);
uint32_t gba_frame_count(const GBA *gba);

#ifdef __cplusplus
}
#endif
