#include "gba_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

void gba_hle_swi(GBA *gba, uint32_t number) {
    ARM7 *cpu = &gba->cpu;
    uint32_t *r = cpu->r;
    switch (number) {
        case 0x02: /* Halt */
            cpu->halt = true;
            break;
        case 0x04: /* IntrWait */
        case 0x05: /* VBlankIntrWait */ {
            uint32_t flags = (number == 0x05) ? 1u : r[1];
            io16_set(gba, 0x208, 1);
            if (r[0] || number == 0x05)
                io16_set(gba, 0x202, (uint16_t)(io16(gba, 0x202) & ~flags));
            cpu->halt = true;
            break;
        }
        case 0x06: { /* Div */
            int32_t n = (int32_t)r[0], d = (int32_t)r[1];
            if (d == 0) break;
            r[0] = (uint32_t)(n / d);
            r[1] = (uint32_t)(n % d);
            r[3] = (uint32_t)abs(n / d);
            break;
        }
        case 0x07: { /* DivArm */
            int32_t n = (int32_t)r[1], d = (int32_t)r[0];
            if (d == 0) break;
            r[0] = (uint32_t)(n / d);
            r[1] = (uint32_t)(n % d);
            r[3] = (uint32_t)abs(n / d);
            break;
        }
        case 0x08: { /* Sqrt */
            r[0] = (uint32_t)sqrt((double)r[0]);
            break;
        }
        case 0x0B:
        case 0x0C: { /* CpuSet / CpuFastSet */
            uint32_t src = r[0], dst = r[1], ctl = r[2];
            uint32_t count = ctl & 0x1FFFFF;
            bool fill = (ctl >> 24) & 1;
            bool word = number == 0x0C || ((ctl >> 26) & 1);
            if (number == 0x0C) {
                count = (count + 7) / 8 * 8;
                word = true;
            }
            for (uint32_t i = 0; i < count; i++) {
                if (word) {
                    uint32_t v = gba_read32(gba, fill ? src : src + i * 4);
                    gba_write32(gba, dst + i * 4, v);
                } else {
                    uint16_t v = (uint16_t)gba_read16(gba, fill ? src : src + i * 2);
                    gba_write16(gba, dst + i * 2, v);
                }
            }
            break;
        }
        case 0x11: /* LZ77UnCompWrite8bit */
        case 0x12: { /* LZ77UnCompWrite16bit */
            uint32_t src = r[0], dst = r[1];
            uint32_t header = gba_read32(gba, src);
            uint32_t size = header >> 8;
            src += 4;
            uint32_t written = 0;
            while (written < size) {
                uint8_t flags = (uint8_t)gba_read8(gba, src++);
                for (int b = 0; b < 8 && written < size; b++) {
                    if (flags & 0x80) {
                        uint8_t a = (uint8_t)gba_read8(gba, src++);
                        uint8_t c = (uint8_t)gba_read8(gba, src++);
                        int len = (a >> 4) + 3;
                        int disp = ((a & 0xF) << 8 | c) + 1;
                        for (int i = 0; i < len && written < size; i++) {
                            uint8_t v = (uint8_t)gba_read8(gba, dst - disp);
                            gba_write8(gba, dst++, v);
                            written++;
                        }
                    } else {
                        gba_write8(gba, dst++, (uint8_t)gba_read8(gba, src++));
                        written++;
                    }
                    flags <<= 1;
                }
            }
            break;
        }
        case 0x01: /* RegisterRamReset */
            if (r[0] & 1) memset(gba->ewram, 0, sizeof gba->ewram);
            if (r[0] & 2) memset(gba->iwram, 0, 0x7E00);
            break;
        default:
            break;
    }
}

static void update_dispstat(GBA *gba) {
    uint16_t ds = io16(gba, 0x004);
    ds &= ~7;
    if (gba->in_vblank) ds |= 1;
    if (gba->in_hblank) ds |= 2;
    uint8_t vct = (uint8_t)(ds >> 8);
    if (gba->scanline == vct) ds |= 4;
    io16_set(gba, 0x004, ds);
    io16_set(gba, 0x006, (uint16_t)gba->scanline);
}

static void run_cycles(GBA *gba, int budget) {
    int left = budget;
    while (left > 0) {
        gba_check_irq(gba);
        int c = cpu_step(gba);
        if (c < 1) c = 1;
        timers_add_cycles(gba, c);
        gba->audio_accum += c;
        /* ~280896 cycles/frame, 8192 samples/sec-ish: 32768 Hz => ~8.56 cycles/sample */
        while (gba->audio_accum >= 512.0) {
            gba->audio_accum -= 512.0;
            apu_push_sample(gba);
        }
        left -= c;
        if (gba->cpu.halt) {
            timers_add_cycles(gba, left);
            break;
        }
    }
}

static void latch_affine(GBA *gba) {
    gba->bg2x = io32(gba, 0x028) & 0x0FFFFFFF;
    gba->bg2y = io32(gba, 0x02C) & 0x0FFFFFFF;
    gba->bg3x = io32(gba, 0x038) & 0x0FFFFFFF;
    gba->bg3y = io32(gba, 0x03C) & 0x0FFFFFFF;
    gba->bg2x_int = ((int32_t)(gba->bg2x << 4)) >> 4;
    gba->bg2y_int = ((int32_t)(gba->bg2y << 4)) >> 4;
    gba->bg3x_int = ((int32_t)(gba->bg3x << 4)) >> 4;
    gba->bg3y_int = ((int32_t)(gba->bg3y << 4)) >> 4;
}

void gba_run_frame(GBA *gba) {
    gba->audio_len = 0;
    for (int line = 0; line < GBA_FRAME_LINES; line++) {
        gba->scanline = line;
        gba->in_hblank = false;
        gba->in_vblank = line >= GBA_VISIBLE_LINES;
        if (line == 0)
            latch_affine(gba);
        if (line == GBA_VISIBLE_LINES) {
            gba->in_vblank = true;
            update_dispstat(gba);
            uint16_t ds = io16(gba, 0x004);
            if (ds & (1 << 3))
                gba_raise_irq(gba, 1);
            dma_on_vblank(gba);
        }
        update_dispstat(gba);
        uint16_t ds = io16(gba, 0x004);
        if ((ds & 4) && (ds & (1 << 5)))
            gba_raise_irq(gba, 1 << 2);

        run_cycles(gba, GBA_HDRAW_DOTS * GBA_CYCLES_PER_DOT);
        if (line < GBA_VISIBLE_LINES)
            ppu_render_scanline(gba, line);

        gba->in_hblank = true;
        update_dispstat(gba);
        ds = io16(gba, 0x004);
        if (ds & (1 << 4))
            gba_raise_irq(gba, 1 << 1);
        if (line < GBA_VISIBLE_LINES)
            dma_on_hblank(gba);
        run_cycles(gba, GBA_HBLANK_DOTS * GBA_CYCLES_PER_DOT);

        int32_t pb = (int16_t)io16(gba, 0x022);
        int32_t pd = (int16_t)io16(gba, 0x026);
        gba->bg2x_int += pb;
        gba->bg2y_int += pd;
        pb = (int16_t)io16(gba, 0x032);
        pd = (int16_t)io16(gba, 0x036);
        gba->bg3x_int += pb;
        gba->bg3y_int += pd;
    }
    gba->frame++;
}

GBA *gba_create(void) {
    GBA *gba = calloc(1, sizeof(GBA));
    if (!gba) return NULL;
    /* Tiny IRQ stub at 0x18: the CPU HLE intercepts this range. */
    gba->bios[0x18] = 0x12;
    gba->bios[0x19] = 0x00;
    gba->bios[0x1A] = 0x00;
    gba->bios[0x1B] = 0xEA;
    gba_reset(gba);
    return gba;
}

void gba_destroy(GBA *gba) {
    if (!gba) return;
    free(gba->rom);
    free(gba);
}

void gba_reset(GBA *gba) {
    uint8_t *rom = gba->rom;
    size_t sz = gba->rom_size;
    char title[13];
    memcpy(title, gba->title, 13);
    memset(gba->ewram, 0, sizeof gba->ewram);
    memset(gba->iwram, 0, sizeof gba->iwram);
    memset(gba->io, 0, sizeof gba->io);
    memset(gba->pal, 0, sizeof gba->pal);
    memset(gba->vram, 0, sizeof gba->vram);
    memset(gba->oam, 0, sizeof gba->oam);
    memset(&gba->dma, 0, sizeof gba->dma);
    memset(&gba->timer, 0, sizeof gba->timer);
    gba->rom = rom;
    gba->rom_size = sz;
    memcpy(gba->title, title, 13);
    io16_set(gba, 0x000, 0x0080);
    io16_set(gba, 0x020, 0x0100);
    io16_set(gba, 0x026, 0x0100);
    io16_set(gba, 0x030, 0x0100);
    io16_set(gba, 0x036, 0x0100);
    io16_set(gba, 0x130, 0x03FF);
    io16_set(gba, 0x088, 0x0200);
    gba->keys = 0;
    gba->frame = 0;
    gba->scanline = 0;
    gba->fifo_a_len = gba->fifo_b_len = 0;
    cpu_reset(gba);
}

bool gba_load_rom(GBA *gba, const uint8_t *data, size_t size) {
    if (!data || size < 4) return false;
    free(gba->rom);
    gba->rom = malloc(size);
    if (!gba->rom) return false;
    memcpy(gba->rom, data, size);
    gba->rom_size = size;
    memset(gba->title, 0, sizeof gba->title);
    if (size >= 0xA0 + 12)
        memcpy(gba->title, data + 0xA0, 12);
    memset(gba->sram, 0xFF, sizeof gba->sram);
    gba_reset(gba);
    return true;
}

void gba_load_demo(GBA *gba) {
    uint8_t rom[0x200];
    memset(rom, 0, sizeof rom);
    /* b 0xC0 */
    rom[0] = 0x2E; rom[1] = 0x00; rom[2] = 0x00; rom[3] = 0xEA;
    memcpy(rom + 0xA0, "MACGBA DEMO", 11);
    rom[0xB2] = 0x96;
    static const uint32_t code[] = {
        0xE3A00404, /* mov r0, #0x04000000 */
        0xE3A01003, /* mov r1, #3 */
        0xE3811B01, /* orr r1, r1, #0x400  DISPCNT mode3+BG2 */
        0xE1C010B0, /* strh r1, [r0] */
        0xE3A02506, /* mov r2, #0x06000000 */
        0xE3A03C96, /* mov r3, #38400 */
        0xE3A04000, /* mov r4, #0 */
        0xE1C240B0, /* strh r4, [r2] */
        0xE2822002, /* add r2, r2, #2 */
        0xE2844001, /* add r4, r4, #1 */
        0xE2533001, /* subs r3, r3, #1 */
        0x1AFFFFFA, /* bne loop */
        0xEAFFFFFE  /* b . */
    };
    memcpy(rom + 0xC0, code, sizeof code);
    gba_load_rom(gba, rom, sizeof rom);
}

const uint32_t *gba_framebuffer(const GBA *gba) { return gba->framebuffer; }
void gba_set_keys(GBA *gba, uint16_t keys_pressed) { gba->keys = keys_pressed & 0x3FF; }
const char *gba_rom_title(const GBA *gba) { return gba->title; }
uint32_t gba_frame_count(const GBA *gba) { return gba->frame; }
