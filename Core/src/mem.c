#include "gba_internal.h"
#include <string.h>

static uint32_t open_bus(GBA *gba) {
    return gba->last_prefetch;
}

static uint8_t *vram_ptr(GBA *gba, uint32_t addr, int *size) {
    addr &= 0x1FFFF;
    if (addr >= 0x18000) addr -= 0x8000;
    *size = 0x18000;
    return gba->vram + addr;
}

uint32_t gba_read8(GBA *gba, uint32_t addr) {
    switch (addr >> 24) {
        case 0x00:
            if (addr < 0x4000) return gba->bios[addr];
            return open_bus(gba) & 0xFF;
        case 0x02:
            return gba->ewram[addr & 0x3FFFF];
        case 0x03:
            return gba->iwram[addr & 0x7FFF];
        case 0x04:
            if ((addr & 0xFFFF) < 0x400)
                return (uint8_t)gba_io_read16(gba, addr & ~1u) >> ((addr & 1) * 8);
            return open_bus(gba) & 0xFF;
        case 0x05:
            return gba->pal[addr & 0x3FF];
        case 0x06: {
            int sz;
            uint8_t *p = vram_ptr(gba, addr, &sz);
            (void)sz;
            return *p;
        }
        case 0x07:
            return gba->oam[addr & 0x3FF];
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
            if (gba->rom) {
                uint32_t off = addr & 0x01FFFFFF;
                if (off < gba->rom_size) return gba->rom[off];
            }
            return 0;
        case 0x0E:
        case 0x0F:
            return gba->sram[addr & 0xFFFF];
        default:
            return open_bus(gba) & 0xFF;
    }
}

uint32_t gba_read16(GBA *gba, uint32_t addr) {
    addr &= ~1u;
    switch (addr >> 24) {
        case 0x00:
            if (addr < 0x4000)
                return gba->bios[addr] | ((uint32_t)gba->bios[addr + 1] << 8);
            return open_bus(gba) & 0xFFFF;
        case 0x02: {
            uint32_t a = addr & 0x3FFFF;
            return gba->ewram[a] | ((uint32_t)gba->ewram[a + 1] << 8);
        }
        case 0x03: {
            uint32_t a = addr & 0x7FFF;
            return gba->iwram[a] | ((uint32_t)gba->iwram[a + 1] << 8);
        }
        case 0x04:
            return gba_io_read16(gba, addr);
        case 0x05: {
            uint32_t a = addr & 0x3FF;
            return gba->pal[a] | ((uint32_t)gba->pal[a + 1] << 8);
        }
        case 0x06: {
            int sz;
            uint8_t *p = vram_ptr(gba, addr, &sz);
            return p[0] | ((uint32_t)p[1] << 8);
        }
        case 0x07: {
            uint32_t a = addr & 0x3FF;
            return gba->oam[a] | ((uint32_t)gba->oam[a + 1] << 8);
        }
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
            if (gba->rom) {
                uint32_t off = addr & 0x01FFFFFF;
                if (off + 1 < gba->rom_size)
                    return gba->rom[off] | ((uint32_t)gba->rom[off + 1] << 8);
            }
            return 0;
        case 0x0E:
        case 0x0F: {
            uint32_t a = addr & 0xFFFF;
            uint8_t v = gba->sram[a];
            return v | ((uint32_t)v << 8);
        }
        default:
            return open_bus(gba) & 0xFFFF;
    }
}

uint32_t gba_read32(GBA *gba, uint32_t addr) {
    uint32_t unalign = addr & 3;
    addr &= ~3u;
    uint32_t lo = gba_read16(gba, addr);
    uint32_t hi = gba_read16(gba, addr + 2);
    uint32_t v = lo | (hi << 16);
    if (unalign)
        v = (v >> (8 * unalign)) | (v << (32 - 8 * unalign));
    return v;
}

void gba_write8(GBA *gba, uint32_t addr, uint32_t value) {
    uint8_t v = (uint8_t)value;
    switch (addr >> 24) {
        case 0x02:
            gba->ewram[addr & 0x3FFFF] = v;
            break;
        case 0x03:
            gba->iwram[addr & 0x7FFF] = v;
            break;
        case 0x04:
            gba_io_write8(gba, addr, v);
            break;
        case 0x05:
            /* 8-bit palette writes ignored on real GBA except via DMA quirks */
            break;
        case 0x06: {
            int sz;
            uint8_t *p = vram_ptr(gba, addr, &sz);
            (void)sz;
            *p = v;
            break;
        }
        case 0x07:
            break;
        case 0x0E:
        case 0x0F:
            gba->sram[addr & 0xFFFF] = v;
            break;
        default:
            break;
    }
}

void gba_write16(GBA *gba, uint32_t addr, uint32_t value) {
    addr &= ~1u;
    uint16_t v = (uint16_t)value;
    switch (addr >> 24) {
        case 0x02: {
            uint32_t a = addr & 0x3FFFF;
            gba->ewram[a] = (uint8_t)v;
            gba->ewram[a + 1] = (uint8_t)(v >> 8);
            break;
        }
        case 0x03: {
            uint32_t a = addr & 0x7FFF;
            gba->iwram[a] = (uint8_t)v;
            gba->iwram[a + 1] = (uint8_t)(v >> 8);
            break;
        }
        case 0x04:
            gba_io_write16(gba, addr, v);
            break;
        case 0x05: {
            uint32_t a = addr & 0x3FF;
            gba->pal[a] = (uint8_t)v;
            gba->pal[a + 1] = (uint8_t)(v >> 8);
            break;
        }
        case 0x06: {
            int sz;
            uint8_t *p = vram_ptr(gba, addr, &sz);
            p[0] = (uint8_t)v;
            p[1] = (uint8_t)(v >> 8);
            break;
        }
        case 0x07: {
            uint32_t a = addr & 0x3FF;
            gba->oam[a] = (uint8_t)v;
            gba->oam[a + 1] = (uint8_t)(v >> 8);
            break;
        }
        case 0x0E:
        case 0x0F:
            gba->sram[addr & 0xFFFF] = (uint8_t)v;
            gba->sram[(addr + 1) & 0xFFFF] = (uint8_t)(v >> 8);
            break;
        default:
            break;
    }
}

void gba_write32(GBA *gba, uint32_t addr, uint32_t value) {
    addr &= ~3u;
    gba_write16(gba, addr, (uint16_t)value);
    gba_write16(gba, addr + 2, (uint16_t)(value >> 16));
}

uint16_t gba_io_read16(GBA *gba, uint32_t addr) {
    uint32_t off = addr & 0x3FE;
    switch (off) {
        case 0x000:
        case 0x004:
        case 0x006:
            io16_set(gba, 0x006, (uint16_t)gba->scanline);
            break;
        case 0x130:
            return (uint16_t)(~gba->keys) & 0x3FF;
        case 0x100:
        case 0x104:
        case 0x108:
        case 0x10C: {
            int t = (int)((off - 0x100) / 4);
            return gba->timer[t].counter;
        }
        default:
            break;
    }
    return io16(gba, off);
}

void gba_io_write8(GBA *gba, uint32_t addr, uint8_t v) {
    uint32_t aligned = addr & ~1u;
    uint16_t cur = gba_io_read16(gba, aligned);
    if (addr & 1)
        cur = (uint16_t)((cur & 0x00FF) | ((uint16_t)v << 8));
    else
        cur = (uint16_t)((cur & 0xFF00) | v);
    gba_io_write16(gba, aligned, cur);
}

static void latch_timer(GBA *gba, int t, uint16_t control) {
    Timer *tm = &gba->timer[t];
    uint16_t old = tm->control;
    tm->control = control;
    if ((control & 0x80) && !(old & 0x80)) {
        tm->counter = tm->reload;
        tm->remainder = 0;
    }
}

static void fifo_push(uint8_t *fifo, int *len, uint8_t byte) {
    if (*len < 32)
        fifo[(*len)++] = byte;
}

void gba_io_write16(GBA *gba, uint32_t addr, uint16_t v) {
    uint32_t off = addr & 0x3FE;
    switch (off) {
        case 0x004: {
            uint16_t old = io16(gba, 0x004);
            io16_set(gba, 0x004, (uint16_t)((v & ~0x0007) | (old & 0x0007)));
            return;
        }
        case 0x006:
            return;
        case 0x0A0:
        case 0x0A2:
            fifo_push(gba->fifo_a, &gba->fifo_a_len, (uint8_t)v);
            fifo_push(gba->fifo_a, &gba->fifo_a_len, (uint8_t)(v >> 8));
            return;
        case 0x0A4:
        case 0x0A6:
            fifo_push(gba->fifo_b, &gba->fifo_b_len, (uint8_t)v);
            fifo_push(gba->fifo_b, &gba->fifo_b_len, (uint8_t)(v >> 8));
            return;
        case 0x0BA:
        case 0x0C6:
        case 0x0D2:
        case 0x0DE: {
            io16_set(gba, off, v);
            int ch = (int)((off - 0x0BA) / 12);
            dma_try_start(gba, ch);
            return;
        }
        case 0x100: gba->timer[0].reload = v; return;
        case 0x104: gba->timer[1].reload = v; return;
        case 0x108: gba->timer[2].reload = v; return;
        case 0x10C: gba->timer[3].reload = v; return;
        case 0x102: latch_timer(gba, 0, v); io16_set(gba, off, v); return;
        case 0x106: latch_timer(gba, 1, v); io16_set(gba, off, v); return;
        case 0x10A: latch_timer(gba, 2, v); io16_set(gba, off, v); return;
        case 0x10E: latch_timer(gba, 3, v); io16_set(gba, off, v); return;
        case 0x202: {
            uint16_t iff = io16(gba, 0x202);
            io16_set(gba, 0x202, (uint16_t)(iff & ~v));
            return;
        }
        case 0x208:
            io16_set(gba, 0x208, v & 1);
            gba_check_irq(gba);
            return;
        case 0x200:
            io16_set(gba, 0x200, v);
            gba_check_irq(gba);
            return;
        case 0x301:
            if ((v & 0x80) == 0)
                gba->cpu.halt = true;
            io16_set(gba, off, v);
            return;
        default:
            io16_set(gba, off, v);
            return;
    }
}
