#include "gba_internal.h"
#include <string.h>

static uint16_t pal16(const GBA *gba, int index) {
    index &= 0x1FF;
    return (uint16_t)(gba->pal[index * 2] | (gba->pal[index * 2 + 1] << 8));
}

static uint32_t to_bgra(uint16_t c) {
    uint32_t r = (c & 31) << 3;
    uint32_t g = ((c >> 5) & 31) << 3;
    uint32_t b = ((c >> 10) & 31) << 3;
    r |= r >> 5;
    g |= g >> 5;
    b |= b >> 5;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static uint16_t vram16(const GBA *gba, uint32_t addr) {
    addr &= 0x1FFFF;
    if (addr >= 0x18000) addr -= 0x8000;
    return (uint16_t)(gba->vram[addr] | (gba->vram[addr + 1] << 8));
}

static uint8_t vram8(const GBA *gba, uint32_t addr) {
    addr &= 0x1FFFF;
    if (addr >= 0x18000) addr -= 0x8000;
    return gba->vram[addr];
}

static void draw_text_bg(GBA *gba, int y, int bg, uint16_t *line, uint8_t *prio, uint8_t *layer) {
    uint16_t dispcnt = io16(gba, 0x000);
    uint16_t cnt = io16(gba, 0x008 + bg * 2);
    if (!(dispcnt & (1u << (8 + bg)))) return;
    int prio_v = cnt & 3;
    int cbb = (cnt >> 2) & 3;
    int mosaic = (cnt >> 6) & 1;
    int pal256 = (cnt >> 7) & 1;
    int sbb = (cnt >> 8) & 31;
    int wrap_size = (cnt >> 14) & 3;
    int hofs = io16(gba, 0x010 + bg * 4) & 0x1FF;
    int vofs = io16(gba, 0x012 + bg * 4) & 0x1FF;
    (void)mosaic;
    int map_w = (wrap_size & 1) ? 64 : 32;
    int map_h = (wrap_size & 2) ? 64 : 32;
    int ly = (y + vofs) & (map_h * 8 - 1);
    int ty = ly >> 3;
    int row = ly & 7;
    uint32_t map_base = (uint32_t)sbb * 0x800;
    uint32_t tile_base = (uint32_t)cbb * 0x4000;

    for (int x = 0; x < 240; x++) {
        int lx = (x + hofs) & (map_w * 8 - 1);
        int tx = lx >> 3;
        int col = lx & 7;
        int map_x = tx;
        int map_y = ty;
        uint32_t screen = 0;
        if (map_w == 64) {
            if (map_x >= 32) { screen += 0x800; map_x -= 32; }
        }
        if (map_h == 64 && map_y >= 32) {
            screen += (map_w == 64) ? 0x1000 : 0x800;
            map_y -= 32;
        }
        uint32_t entry_addr = map_base + screen + ((uint32_t)map_y * 32 + (uint32_t)map_x) * 2;
        uint16_t entry = vram16(gba, entry_addr);
        int tile = entry & 0x3FF;
        int hf = (entry >> 10) & 1;
        int vf = (entry >> 11) & 1;
        int pal = (entry >> 12) & 0xF;
        int r = vf ? 7 - row : row;
        int c = hf ? 7 - col : col;
        uint16_t color;
        if (pal256) {
            uint8_t pix = vram8(gba, tile_base + (uint32_t)tile * 64 + (uint32_t)r * 8 + (uint32_t)c);
            if (!pix) continue;
            color = pal16(gba, pix);
        } else {
            uint8_t pack = vram8(gba, tile_base + (uint32_t)tile * 32 + (uint32_t)r * 4 + (uint32_t)(c >> 1));
            uint8_t pix = (c & 1) ? (pack >> 4) : (pack & 0xF);
            if (!pix) continue;
            color = pal16(gba, pal * 16 + pix);
        }
        if (prio_v <= prio[x]) {
            line[x] = color;
            prio[x] = (uint8_t)prio_v;
            layer[x] = (uint8_t)bg;
        }
    }
}

static void draw_affine_bg(GBA *gba, int y, int bg, uint16_t *line, uint8_t *prio, uint8_t *layer) {
    uint16_t dispcnt = io16(gba, 0x000);
    if (!(dispcnt & (1u << (8 + bg)))) return;
    uint16_t cnt = io16(gba, 0x008 + bg * 2);
    int prio_v = cnt & 3;
    int cbb = (cnt >> 2) & 3;
    int pal256 = 1;
    int sbb = (cnt >> 8) & 31;
    int wrap = (cnt >> 13) & 1;
    int size = (cnt >> 14) & 3;
    int dim = 16 << size;
    (void)pal256;
    int32_t pa = (int16_t)io16(gba, bg == 2 ? 0x020 : 0x030);
    int32_t pc = (int16_t)io16(gba, bg == 2 ? 0x024 : 0x034);
    int32_t x0 = bg == 2 ? gba->bg2x_int : gba->bg3x_int;
    int32_t y0 = bg == 2 ? gba->bg2y_int : gba->bg3y_int;
    uint32_t map_base = (uint32_t)sbb * 0x800;
    uint32_t tile_base = (uint32_t)cbb * 0x4000;

    for (int x = 0; x < 240; x++) {
        int32_t rx = (x0 + pa * x) >> 8;
        int32_t ry = (y0 + pc * x) >> 8;
        if (wrap) {
            rx %= (dim * 8);
            ry %= (dim * 8);
            if (rx < 0) rx += dim * 8;
            if (ry < 0) ry += dim * 8;
        } else if (rx < 0 || ry < 0 || rx >= dim * 8 || ry >= dim * 8) {
            continue;
        }
        int tx = rx >> 3, ty = ry >> 3, c = rx & 7, r = ry & 7;
        uint8_t tile = vram8(gba, map_base + (uint32_t)ty * (uint32_t)dim + (uint32_t)tx);
        uint8_t pix = vram8(gba, tile_base + (uint32_t)tile * 64 + (uint32_t)r * 8 + (uint32_t)c);
        if (!pix) continue;
        uint16_t color = pal16(gba, pix);
        if (prio_v <= prio[x]) {
            line[x] = color;
            prio[x] = (uint8_t)prio_v;
            layer[x] = (uint8_t)bg;
        }
    }
    (void)y;
}

static void draw_bitmap(GBA *gba, int y, uint16_t *line) {
    uint16_t dispcnt = io16(gba, 0x000);
    int mode = dispcnt & 7;
    int frame = (dispcnt >> 4) & 1;
    if (mode == 3) {
        for (int x = 0; x < 240; x++)
            line[x] = vram16(gba, ((uint32_t)y * 240 + (uint32_t)x) * 2);
    } else if (mode == 4) {
        uint32_t base = frame ? 0xA000 : 0;
        for (int x = 0; x < 240; x++) {
            uint8_t idx = vram8(gba, base + (uint32_t)y * 240 + (uint32_t)x);
            line[x] = pal16(gba, idx);
        }
    } else if (mode == 5) {
        uint32_t base = frame ? 0xA000 : 0;
        if (y >= 128) return;
        for (int x = 0; x < 160; x++)
            line[x] = vram16(gba, base + ((uint32_t)y * 160 + (uint32_t)x) * 2);
    }
}

static const int obj_w[4][4] = {
    {8, 16, 32, 64},
    {16, 32, 32, 64},
    {8, 8, 16, 32},
    {8, 8, 8, 8}
};
static const int obj_h[4][4] = {
    {8, 16, 32, 64},
    {8, 8, 16, 32},
    {16, 32, 32, 64},
    {8, 8, 8, 8}
};

static void draw_sprites(GBA *gba, int y, uint16_t *line, uint8_t *prio, uint8_t *is_obj) {
    uint16_t dispcnt = io16(gba, 0x000);
    if (!(dispcnt & (1u << 12))) return;
    int map_1d = (dispcnt >> 6) & 1;
    for (int i = 127; i >= 0; i--) {
        uint16_t a0 = (uint16_t)(gba->oam[i * 8] | (gba->oam[i * 8 + 1] << 8));
        uint16_t a1 = (uint16_t)(gba->oam[i * 8 + 2] | (gba->oam[i * 8 + 3] << 8));
        uint16_t a2 = (uint16_t)(gba->oam[i * 8 + 4] | (gba->oam[i * 8 + 5] << 8));
        int iy = a0 & 0xFF;
        int obj_mode = (a0 >> 8) & 3;
        if (obj_mode == 2) continue;
        int affine = (a0 >> 8) & 1;
        int dbl = (a0 >> 9) & 1;
        int mosaic = (a0 >> 12) & 1;
        int pal256 = (a0 >> 13) & 1;
        int shape = (a0 >> 14) & 3;
        int ix = a1 & 0x1FF;
        int hf = (a1 >> 12) & 1;
        int vf = (a1 >> 13) & 1;
        int size = (a1 >> 14) & 3;
        int tile = a2 & 0x3FF;
        int prio_v = (a2 >> 10) & 3;
        int pal = (a2 >> 12) & 0xF;
        (void)mosaic;
        int w = obj_w[shape][size];
        int h = obj_h[shape][size];
        if (iy >= 160) iy -= 256;
        if (ix >= 240) ix -= 512;
        int aw = w, ah = h;
        if (affine && dbl) { aw *= 2; ah *= 2; }
        if (y < iy || y >= iy + ah) continue;
        int py = y - iy;
        int pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            int aff = (a1 >> 9) & 0x1F;
            pa = (int16_t)(gba->oam[0x06 + aff * 32] | (gba->oam[0x07 + aff * 32] << 8));
            pb = (int16_t)(gba->oam[0x0E + aff * 32] | (gba->oam[0x0F + aff * 32] << 8));
            pc = (int16_t)(gba->oam[0x16 + aff * 32] | (gba->oam[0x17 + aff * 32] << 8));
            pd = (int16_t)(gba->oam[0x1E + aff * 32] | (gba->oam[0x1F + aff * 32] << 8));
        }
        for (int px = 0; px < aw; px++) {
            int sx = ix + px;
            if (sx < 0 || sx >= 240) continue;
            int tx, ty;
            if (affine) {
                int cx = px - aw / 2;
                int cy = py - ah / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = hf ? (w - 1 - px) : px;
                ty = vf ? (h - 1 - py) : py;
            }
            int tile_x = tx >> 3, tile_y = ty >> 3;
            int c = tx & 7, r = ty & 7;
            int stride = map_1d ? (w / 8) : (pal256 ? 16 : 32);
            int t = tile + tile_y * stride + tile_x * (pal256 ? 2 : 1);
            uint16_t color;
            uint32_t base = 0x10000;
            if (pal256) {
                uint8_t pix = vram8(gba, base + (uint32_t)t * 32 + (uint32_t)r * 8 + (uint32_t)c);
                if (!pix) continue;
                color = pal16(gba, 256 + pix);
            } else {
                uint8_t pack = vram8(gba, base + (uint32_t)t * 32 + (uint32_t)r * 4 + (uint32_t)(c >> 1));
                uint8_t pix = (c & 1) ? (pack >> 4) : (pack & 0xF);
                if (!pix) continue;
                color = pal16(gba, 256 + pal * 16 + pix);
            }
            if (prio_v <= prio[sx]) {
                line[sx] = color;
                prio[sx] = (uint8_t)prio_v;
                is_obj[sx] = 1;
            }
        }
    }
}

void ppu_render_scanline(GBA *gba, int y) {
    uint16_t dispcnt = io16(gba, 0x000);
    int mode = dispcnt & 7;
    uint16_t backdrop = pal16(gba, 0);
    uint16_t line[240];
    uint8_t prio[240];
    uint8_t layer[240];
    for (int x = 0; x < 240; x++) {
        line[x] = backdrop;
        prio[x] = 4;
        layer[x] = 5;
    }
    if (mode <= 2) {
        for (int bg = 3; bg >= 0; bg--) {
            if (mode == 1 && bg == 3) continue;
            if (mode == 2 && bg < 2) continue;
            bool affine = (mode == 2) || (mode == 1 && bg == 2) || (mode == 0 ? false : (bg >= 2 && mode == 2));
            if (mode == 0)
                draw_text_bg(gba, y, bg, line, prio, layer);
            else if (mode == 1) {
                if (bg < 2) draw_text_bg(gba, y, bg, line, prio, layer);
                else if (bg == 2) draw_affine_bg(gba, y, 2, line, prio, layer);
            } else {
                if (bg >= 2) draw_affine_bg(gba, y, bg, line, prio, layer);
            }
            (void)affine;
        }
    } else {
        draw_bitmap(gba, y, line);
        for (int x = 0; x < 240; x++) prio[x] = 3;
    }
    uint8_t is_obj[240];
    memset(is_obj, 0, sizeof is_obj);
    draw_sprites(gba, y, line, prio, is_obj);

    uint32_t *out = gba->framebuffer + y * 240;
    for (int x = 0; x < 240; x++)
        out[x] = to_bgra(line[x]);
}

void ppu_vblank_start(GBA *gba) {
    gba->bg2x_int = (int32_t)(gba->bg2x << 4) >> 4;
    gba->bg2y_int = (int32_t)(gba->bg2y << 4) >> 4;
    gba->bg3x_int = (int32_t)(gba->bg3x << 4) >> 4;
    gba->bg3y_int = (int32_t)(gba->bg3y << 4) >> 4;
    if (gba->bg2x == 0) gba->bg2x_int = (int32_t)(io32(gba, 0x028) << 4) >> 4;
    if (gba->bg2y == 0) gba->bg2y_int = (int32_t)(io32(gba, 0x02C) << 4) >> 4;
}
