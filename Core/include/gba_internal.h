#pragma once

#include "gba.h"

#define GBA_CYCLES_PER_DOT 4
#define GBA_HDRAW_DOTS 240
#define GBA_HBLANK_DOTS 68
#define GBA_DOTS_PER_LINE 308
#define GBA_CYCLES_PER_LINE (GBA_DOTS_PER_LINE * GBA_CYCLES_PER_DOT)
#define GBA_VISIBLE_LINES 160
#define GBA_FRAME_LINES 228

#define MODE_USR 0x10
#define MODE_FIQ 0x11
#define MODE_IRQ 0x12
#define MODE_SVC 0x13
#define MODE_ABT 0x17
#define MODE_UND 0x1B
#define MODE_SYS 0x1F

#define PSR_N 0x80000000u
#define PSR_Z 0x40000000u
#define PSR_C 0x20000000u
#define PSR_V 0x10000000u
#define PSR_I 0x00000080u
#define PSR_F 0x00000040u
#define PSR_T 0x00000020u
#define PSR_MODE 0x0000001Fu

typedef struct {
    uint32_t r[16];
    uint32_t cpsr;
    uint32_t spsr;
    uint32_t bank_r8_fiq[5];
    uint32_t bank_usr[2];
    uint32_t bank_fiq[2];
    uint32_t bank_svc[2];
    uint32_t bank_abt[2];
    uint32_t bank_irq[2];
    uint32_t bank_und[2];
    uint32_t spsr_fiq, spsr_svc, spsr_abt, spsr_irq, spsr_und;
    bool halt;
} ARM7;

typedef struct {
    uint32_t src, dst, cnt;
    uint32_t src_reload, dst_reload;
} DMAChannel;

typedef struct {
    uint16_t reload;
    uint16_t counter;
    uint16_t control;
    uint32_t remainder;
} Timer;

struct GBA {
    ARM7 cpu;
    uint8_t bios[0x4000];
    uint8_t ewram[0x40000];
    uint8_t iwram[0x8000];
    uint8_t io[0x400];
    uint8_t pal[0x400];
    uint8_t vram[0x18000];
    uint8_t oam[0x400];
    uint8_t sram[0x10000];
    uint8_t *rom;
    size_t rom_size;
    char title[13];

    uint32_t framebuffer[GBA_FRAMEBUFFER_PIXELS];
    uint16_t keys;
    uint32_t frame;
    uint32_t cycles_in_line;
    int scanline;
    bool in_hblank;
    bool in_vblank;

    DMAChannel dma[4];
    Timer timer[4];

    uint8_t fifo_a[32];
    uint8_t fifo_b[32];
    int fifo_a_len, fifo_b_len;
    int8_t fifo_a_sample, fifo_b_sample;
    float audio[2048];
    int audio_len;
    double audio_accum;
    uint32_t last_prefetch;

    uint32_t bg2x, bg2y, bg3x, bg3y;
    int32_t bg2x_int, bg2y_int, bg3x_int, bg3y_int;
};

uint32_t gba_read8(GBA *gba, uint32_t addr);
uint32_t gba_read16(GBA *gba, uint32_t addr);
uint32_t gba_read32(GBA *gba, uint32_t addr);
void gba_write8(GBA *gba, uint32_t addr, uint32_t value);
void gba_write16(GBA *gba, uint32_t addr, uint32_t value);
void gba_write32(GBA *gba, uint32_t addr, uint32_t value);

void gba_io_write8(GBA *gba, uint32_t addr, uint8_t v);
void gba_io_write16(GBA *gba, uint32_t addr, uint16_t v);
uint16_t gba_io_read16(GBA *gba, uint32_t addr);

void cpu_reset(GBA *gba);
int cpu_step(GBA *gba);
void cpu_interrupt(GBA *gba, uint32_t vector);
void cpu_switch_mode(GBA *gba, uint32_t mode);
void cpu_set_cpsr(GBA *gba, uint32_t value, bool flags, bool control);

void ppu_render_scanline(GBA *gba, int y);
void ppu_vblank_start(GBA *gba);

void dma_try_start(GBA *gba, int ch);
void dma_on_vblank(GBA *gba);
void dma_on_hblank(GBA *gba);
void dma_on_fifo(GBA *gba, int fifo /* 0=A,1=B */);

void timers_add_cycles(GBA *gba, int cycles);
void apu_push_sample(GBA *gba);
void apu_on_timer_overflow(GBA *gba, int timer);

void gba_raise_irq(GBA *gba, uint16_t bit);
void gba_check_irq(GBA *gba);
void gba_hle_swi(GBA *gba, uint32_t number);

static inline uint16_t io16(const GBA *gba, uint32_t off) {
    return (uint16_t)gba->io[off] | ((uint16_t)gba->io[off + 1] << 8);
}
static inline void io16_set(GBA *gba, uint32_t off, uint16_t v) {
    gba->io[off] = (uint8_t)v;
    gba->io[off + 1] = (uint8_t)(v >> 8);
}
static inline uint32_t io32(const GBA *gba, uint32_t off) {
    return gba->io[off] | ((uint32_t)gba->io[off + 1] << 8) |
           ((uint32_t)gba->io[off + 2] << 16) | ((uint32_t)gba->io[off + 3] << 24);
}
static inline void io32_set(GBA *gba, uint32_t off, uint32_t v) {
    gba->io[off] = (uint8_t)v;
    gba->io[off + 1] = (uint8_t)(v >> 8);
    gba->io[off + 2] = (uint8_t)(v >> 16);
    gba->io[off + 3] = (uint8_t)(v >> 24);
}
