#include "gba_internal.h"
#include <string.h>

void gba_raise_irq(GBA *gba, uint16_t bit) {
    io16_set(gba, 0x202, io16(gba, 0x202) | bit);
    gba_check_irq(gba);
}

void gba_check_irq(GBA *gba) {
    if (!(io16(gba, 0x208) & 1)) return;
    if (!(io16(gba, 0x200) & io16(gba, 0x202))) return;
    if (gba->cpu.cpsr & PSR_I) return;
    cpu_interrupt(gba, 0x00000018);
}

static void dma_run(GBA *gba, int ch) {
    DMAChannel *d = &gba->dma[ch];
    uint32_t dad = io32(gba, 0x0B4 + 12 * ch);
    uint32_t sad = io32(gba, 0x0B0 + 12 * ch);
    uint16_t cnt_l = io16(gba, 0x0B8 + 12 * ch);
    uint16_t cnt_h = io16(gba, 0x0BA + 12 * ch);
    uint32_t count = cnt_l ? cnt_l : (ch == 3 ? 0x10000u : 0x4000u);
    int dst_adj = (cnt_h >> 5) & 3;
    int src_adj = (cnt_h >> 7) & 3;
    int width = (cnt_h & (1 << 10)) ? 4 : 2;
    bool fifo = ((cnt_h >> 12) & 3) == 3 && ch >= 1 && ch <= 2;
    if (fifo) {
        count = 4;
        width = 4;
        dst_adj = 2; /* fixed */
    }
    int src_step = (src_adj == 0) ? width : (src_adj == 1) ? -width : 0;
    int dst_step = (dst_adj == 0) ? width : (dst_adj == 1) ? -width : 0;
    sad &= (width == 4) ? ~3u : ~1u;
    dad &= (width == 4) ? ~3u : ~1u;
    for (uint32_t i = 0; i < count; i++) {
        if (width == 4)
            gba_write32(gba, dad, gba_read32(gba, sad));
        else
            gba_write16(gba, dad, (uint16_t)gba_read16(gba, sad));
        sad += src_step;
        dad += dst_step;
    }
    if (dst_adj == 3)
        io32_set(gba, 0x0B4 + 12 * ch, d->dst_reload ? d->dst_reload : io32(gba, 0x0B4 + 12 * ch));
    else
        io32_set(gba, 0x0B4 + 12 * ch, dad);
    io32_set(gba, 0x0B0 + 12 * ch, sad);
    if (!(cnt_h & (1 << 9))) {
        cnt_h &= ~(1u << 15);
        io16_set(gba, 0x0BA + 12 * ch, cnt_h);
    }
    if (cnt_h & (1 << 14))
        gba_raise_irq(gba, (uint16_t)(1 << (8 + ch)));
}

void dma_try_start(GBA *gba, int ch) {
    uint16_t cnt_h = io16(gba, 0x0BA + 12 * ch);
    if (!(cnt_h & (1u << 15))) return;
    int start = (cnt_h >> 12) & 3;
    gba->dma[ch].src_reload = io32(gba, 0x0B0 + 12 * ch);
    gba->dma[ch].dst_reload = io32(gba, 0x0B4 + 12 * ch);
    if (start == 0)
        dma_run(gba, ch);
}

void dma_on_vblank(GBA *gba) {
    for (int ch = 0; ch < 4; ch++) {
        uint16_t cnt_h = io16(gba, 0x0BA + 12 * ch);
        if ((cnt_h & (1u << 15)) && ((cnt_h >> 12) & 3) == 1)
            dma_run(gba, ch);
    }
}

void dma_on_hblank(GBA *gba) {
    for (int ch = 0; ch < 4; ch++) {
        uint16_t cnt_h = io16(gba, 0x0BA + 12 * ch);
        if ((cnt_h & (1u << 15)) && ((cnt_h >> 12) & 3) == 2)
            dma_run(gba, ch);
    }
}

void dma_on_fifo(GBA *gba, int fifo) {
    int ch = fifo ? 2 : 1;
    uint16_t cnt_h = io16(gba, 0x0BA + 12 * ch);
    if ((cnt_h & (1u << 15)) && ((cnt_h >> 12) & 3) == 3)
        dma_run(gba, ch);
}

static const int timer_shift[4] = {0, 6, 8, 10};

void timers_add_cycles(GBA *gba, int cycles) {
    for (int i = 0; i < 4; i++) {
        Timer *t = &gba->timer[i];
        if (!(t->control & 0x80)) continue;
        if (t->control & 0x4) continue; /* count-up */
        t->remainder += (uint32_t)cycles;
        uint32_t step = 1u << timer_shift[t->control & 3];
        while (t->remainder >= step) {
            t->remainder -= step;
            t->counter++;
            if (t->counter == 0) {
                t->counter = t->reload;
                if (t->control & 0x40)
                    gba_raise_irq(gba, (uint16_t)(1 << (3 + i)));
                apu_on_timer_overflow(gba, i);
                if (i < 3 && (gba->timer[i + 1].control & 0x84) == 0x84) {
                    gba->timer[i + 1].counter++;
                    if (gba->timer[i + 1].counter == 0) {
                        gba->timer[i + 1].counter = gba->timer[i + 1].reload;
                        if (gba->timer[i + 1].control & 0x40)
                            gba_raise_irq(gba, (uint16_t)(1 << (4 + i)));
                    }
                }
            }
        }
    }
}

void apu_on_timer_overflow(GBA *gba, int timer) {
    uint16_t snd = io16(gba, 0x082);
    int ta = (snd >> 10) & 1;
    int tb = (snd >> 14) & 1;
    if (timer == ta && (snd & (1 << 8))) {
        if (gba->fifo_a_len > 0) {
            gba->fifo_a_sample = (int8_t)gba->fifo_a[0];
            memmove(gba->fifo_a, gba->fifo_a + 1, (size_t)--gba->fifo_a_len);
        }
        if (gba->fifo_a_len <= 16)
            dma_on_fifo(gba, 0);
    }
    if (timer == tb && (snd & (1 << 12))) {
        if (gba->fifo_b_len > 0) {
            gba->fifo_b_sample = (int8_t)gba->fifo_b[0];
            memmove(gba->fifo_b, gba->fifo_b + 1, (size_t)--gba->fifo_b_len);
        }
        if (gba->fifo_b_len <= 16)
            dma_on_fifo(gba, 1);
    }
}

void apu_push_sample(GBA *gba) {
    float mix = ((float)gba->fifo_a_sample + (float)gba->fifo_b_sample) / 256.0f;
    if (gba->audio_len < 2048)
        gba->audio[gba->audio_len++] = mix * 0.35f;
}

int gba_audio_samples(GBA *gba, float *out, int max_frames) {
    int n = gba->audio_len;
    if (n > max_frames) n = max_frames;
    memcpy(out, gba->audio, (size_t)n * sizeof(float));
    memmove(gba->audio, gba->audio + n, (size_t)(gba->audio_len - n) * sizeof(float));
    gba->audio_len -= n;
    return n;
}
