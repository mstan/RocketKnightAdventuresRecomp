/* RKA USA custom scene renderer.
 * Presentation follows the shared Genesis GameVideo contract used by the
 * Sonic renderers. RKA's data formats and audited instruction sites are
 * independent: terrain $2D7E/$32B6, actors $1FB0, placements $113D8.
 * See CUSTOM-VIDEO.md for addresses, contracts and validation.
 */
#include "rka_video.h"
#include "genesis_runtime.h"
#include "video/genesis_vdp.h"
#include "video/genesis_dac.h"
#include "cmd_server.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VIDEO_OFF, VIDEO_FIT, VIDEO_RATIO, MAX_SPRITES = 2048 };
static int s_mode, s_requested_width = 320, s_expanded = 1;
static double s_ratio = 16.0 / 9.0;
typedef struct {
    int x, y;
    uint16_t attr, fraction_x;
    uint8_t size, screen, front, castle_parallax;
} Sprite;
typedef struct {
    uint8_t ram[65536], sat[640];
    Sprite sprites[MAX_SPRITES];
    unsigned count, serial;
    int camera_x, camera_y, stage;
} Scene;
static Scene s_build, s_history[3], s_presented;
static const Scene *s_scene;
static unsigned s_serial, s_frames, s_terrain_checks, s_terrain_errors, s_spawned;
static unsigned s_sprite_overflow;
static int s_width, s_left, s_origin, s_camera;
static uint8_t *s_priority, *s_occupied;
static int s_capacity;
typedef struct {
    uint32_t phase;
    int32_t camera;
    uint16_t native_h;
    uint8_t shift, valid;
} ScrollStrip;
typedef struct { ScrollStrip strip[32]; unsigned serial, mode, stage; } ScrollFrame;
static ScrollFrame s_scroll[3];
static unsigned s_scroll_serial;
static int s_bg_x;

static unsigned r16(const uint8_t *r, unsigned a)
{ return (r[a & 65535u] << 8) | r[(a + 1) & 65535u]; }
static unsigned r32(const uint8_t *r, unsigned a)
{ return (r16(r, a) << 16) | r16(r, a + 2); }
static unsigned read8(const uint8_t *r, unsigned a)
{
    a &= 0xFFFFFFu;
    return a < 0x100000u ? g_rom[a] : a >= 0xFF0000u ? r[a & 65535u] : 0;
}
static unsigned read16(const uint8_t *r, unsigned a)
{ return (read8(r, a) << 8) | read8(r, a + 1); }
static int scene_active(const uint8_t *r)
{
    unsigned mode = r16(r, 0xB002), map = r16(r, 0xB1D0);
    return (mode == 4 || mode == 7) && map >= 2 && map <= 8 &&
           r32(r, 0xB1F6) && r32(r, 0xB1FA);
}
static int vehicle_active(const uint8_t *r)
{
    unsigned mode = r16(r,0xB002);
    /* The Continue screen retains map mode 1 but invalidates the level's
     * camera limit. Stage 23 is the exceptional, endlessly scrolling city. */
    return (mode == 4 || mode == 7) && r16(r,0xB1D0) == 1 &&
           (r16(r,0xB1E8) < 0x8000 || r16(r,0xFB12) == 23);
}
static int stage_width(const uint8_t *r) { return (int)r16(r, 0xB1E8) + 320; }
static int view_left(int camera, int width, const uint8_t *r)
{
    int left = camera - (width - 320) / 2;
    int end = stage_width(r) - width;
    if (end < 0) end = 0;
    return left < 0 ? 0 : left > end ? end : left;
}
static int configure(const char *mode)
{
    if (!mode) return 0;
    int next;
    double ratio = s_ratio;
    if (!strcmp(mode, "off")) next = VIDEO_OFF;
    else if (!strcmp(mode, "fit") || !strcmp(mode, "adaptive")) next = VIDEO_FIT;
    else {
        char *end;
        double w = strtod(mode, &end);
        if (end == mode || *end != ':') return 0;
        const char *den = end + 1;
        double h = strtod(den, &end);
        if (end == den || *end || !(w > 0) || !(h > 0) ||
            !(w / h > 0) || w / h > (double)INT_MAX / 480.0) return 0;
        ratio = w / h; next = VIDEO_RATIO;
    }
    if (s_mode != next || s_ratio != ratio) s_scene = NULL;
    s_mode = next; s_ratio = ratio;
    return 1;
}
static int enabled(void) { return s_mode != VIDEO_OFF; }
static int width(int dw, int dh, int nw, int nh)
{
    if (!enabled()) return 0;
    double result = s_mode == VIDEO_RATIO ? nh * s_ratio :
                    dw > 0 && dh > 0 ? (double)nh * dw / dh : nw;
    if (result > INT_MAX - 1.0) result = INT_MAX - 1.0;
    int pixels = (int)(result + 0.5);
    s_requested_width = pixels < nw ? nw : pixels;
    return s_requested_width;
}
int rka_video_set_spawn(const char *mode)
{
    if (mode && !strcmp(mode, "expanded")) s_expanded = 1;
    else if (mode && !strcmp(mode, "original")) s_expanded = 0;
    else return 0;
    return 1;
}
const char *rka_video_spawn_mode(void) { return s_expanded ? "expanded" : "original"; }
void rka_video_reset(void)
{
    memset(&s_build, 0, sizeof s_build);
    memset(s_history, 0, sizeof s_history);
    memset(&s_presented, 0, sizeof s_presented);
    s_scene = NULL; s_serial = s_frames = s_spawned = 0;
    s_sprite_overflow = s_terrain_checks = s_terrain_errors = 0;
    memset(s_scroll,0,sizeof s_scroll); s_scroll_serial = 0;
}

/* $69BE/$6AD2/$6BDA/$7964/$803A: D4 is autonomous motion plus
 * D5 >> D3 camera parallax, immediately before NEG/SWAP publishes EC00.
 * Preserve both terms so an edge-clamped wide camera does not drag the
 * background while the foreground is stationary. No guest writes/cycles. */
static void capture_scroll(void)
{
    unsigned address = g_cpu.A[3] & 65535u;
    if (address < 0xEC00 || address >= 0xEC40 || (address & 1)) return;
    unsigned row = (address - 0xEC00) / 2, shift = g_cpu.D[3] & 65535u;
    if (shift > 31) return;
    if (!row) {
        ++s_scroll_serial;
        memset(&s_scroll[s_scroll_serial % 3],0,sizeof s_scroll[0]);
    }
    ScrollFrame *frame = &s_scroll[s_scroll_serial % 3];
    frame->serial = s_scroll_serial;
    frame->mode = r16(g_ram,0xB3B8); frame->stage = r16(g_ram,0xFB12);
    ScrollStrip *strip = &frame->strip[row];
    strip->camera = (int32_t)g_cpu.D[5]; strip->shift = (uint8_t)shift;
    strip->phase = g_cpu.D[4] - (shift ? (uint32_t)(strip->camera >> shift) : 0);
    strip->native_h = (uint16_t)((0u - g_cpu.D[4]) >> 16);
    strip->valid = 1;
}

static int background_scroll(const uint8_t *r, int line, int native_h, int delta)
{
    const ScrollStrip *best = NULL;
    unsigned serial = 0;
    for (unsigned i=0;i<3;++i) {
        const ScrollFrame *f = &s_scroll[i];
        const ScrollStrip *strip = &f->strip[(line & 255) / 8];
        if (f->serial > serial && f->mode == r16(r,0xB3B8) &&
            f->stage == r16(r,0xFB12) && strip->valid &&
            strip->native_h == (uint16_t)native_h) {
            best = strip; serial = f->serial;
        }
    }
    if (!best || !delta || !best->shift) return native_h;
    int64_t camera = best->camera + (int64_t)delta * 65536;
    uint32_t value = best->phase + (uint32_t)(camera >> best->shift);
    return (int16_t)((0u - value) >> 16);
}

/* The native streamers expand one layout byte into a 4x4 array of tile
 * attributes. B1EC is the layout row stride, independent of VDP wrapping. */
static uint16_t terrain(const uint8_t *r, int x, int y)
{
    if (x < 0 || x >= stage_width(r)) return 0;
    unsigned mx = (unsigned)x & (r16(r, 0xB1E4) | 31u);
    unsigned my = (unsigned)y & (r16(r, 0xB1E6) | 31u);
    unsigned block = read8(r, r32(r, 0xB1FA) + (my >> 5) * r16(r, 0xB1EC) + (mx >> 5));
    return (uint16_t)read16(r, r32(r, 0xB1F6) + block * 32u +
                           ((my >> 3) & 3u) * 8u + ((mx >> 3) & 3u) * 2u);
}
static uint8_t pixel(const GVDP *v, unsigned attr, int x, int y)
{
    unsigned px = (unsigned)x & 7u, py = (unsigned)y & 7u;
    if (attr & 0x0800) px ^= 7;
    if (attr & 0x1000) py ^= 7;
    unsigned a = (attr & 2047u) * 32u + py * 4u + px / 2u;
    unsigned byte = v->vram[a & 65535u];
    unsigned nibble = px & 1 ? byte & 15 : byte >> 4;
    return nibble ? (uint8_t)(((attr >> 9) & 48u) | nibble) : 0;
}
static int tiles(unsigned bits) { return bits == 1 ? 64 : bits == 3 ? 128 : 32; }
static uint16_t plane(const GVDP *v, unsigned base, int x, int y)
{
    int wt = tiles(v->reg[16] & 3), ht = tiles((v->reg[16] >> 4) & 3);
    unsigned cell = (((unsigned)y >> 3) & (ht - 1)) * wt + (((unsigned)x >> 3) & (wt - 1));
    return (uint16_t)r16(v->vram, base + cell * 2u);
}
static int unwrap(int value, int reference)
{ return reference + ((value - reference + 512) & 1023) - 512; }

/* Capture the original mapping before BuildSprites applies its 80-piece
 * hardware limit and 9-bit SAT coordinate mask. No guest state is changed. */
static void capture_actor(void)
{
    unsigned o = g_cpu.A[1] & 65535u, map = g_cpu.A[4] & 0xFFFFFFu;
    unsigned type = r16(g_ram,o);
    unsigned count = read16(g_ram, map) + 1;
    if (count > MAX_SPRITES) return;
    int flip_x = g_ram[o + 0x1A] != 0, flip_y = g_ram[o + 0x1B] != 0;
    int screen = (g_ram[o + 0x27] & 4) != 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned p = map + 2 + i * 8;
        int x = (int16_t)read16(g_ram, p), y = (int16_t)read16(g_ram, p + 2);
        unsigned flags = read16(g_ram, p + 4), size = flags & 15;
        unsigned attr = read16(g_ram, p + 6) & 2047;
        if (((flags & 16) != 0) ^ flip_x) attr |= 0x0800;
        if (((flags & 128) != 0) ^ flip_y) attr |= 0x1000;
        attr |= (g_ram[o + 0x19] & 128) ? (g_ram[o + 0x19] & 3u) << 13 : (flags & 96u) << 8;
        if (g_ram[o + 0x18]) attr |= 0x8000;
        if (flip_x) x = -x - (((size >> 2) & 3) + 1) * 8;
        if (flip_y) y = -y - ((size & 3) + 1) * 8;
        if (screen) {
            x += (int16_t)r16(g_ram, o + 0x28) - 128;
            y += (int16_t)r16(g_ram, o + 0x2C) - 128;
        } else {
            x += r16(g_ram,o+6); y += r16(g_ram,o+10);
        }
        if (s_build.count < MAX_SPRITES) {
            Sprite *s = &s_build.sprites[s_build.count++];
            s->x = x; s->y = y; s->attr = (uint16_t)attr;
            s->size = (uint8_t)size; s->screen = (uint8_t)screen;
            s->front = o >= (r32(g_ram,0xB054) & 65535u);
            /* Types $07/$3B are scenery, not ordinary world-space actors.
             * $12C62 computes 7/8 * (camera + 160) for the castle; $12CD8
             * derives its flying detail from that anchor plus a timed phase.
             * Keep the fractional coordinate so presentation rounds only
             * after replacing the camera term. Guest positions stay intact. */
            s->castle_parallax = type == 0x07 || type == 0x3B;
            s->fraction_x = (uint16_t)r16(g_ram,o+8);
        } else ++s_sprite_overflow;
    }
}

static void compare_word(int lhs, int rhs)
{
    uint16_t a = (uint16_t)lhs, b = (uint16_t)rhs, d = (uint16_t)(a - b);
    unsigned flags = (d & 0x8000 ? SR_N : 0) | (!d ? SR_Z : 0) |
                     (((a ^ b) & (a ^ d) & 0x8000) ? SR_V : 0) | (a < b ? SR_C : 0);
    g_cpu.SR = (uint16_t)((g_cpu.SR & ~15u) | flags);
}
static void spawn_visible(void)
{
    /* $113D8 scans at most three 256px placement columns. Iterate the wider
     * range through the SAME native cell loaders, allocator and remembered
     * placement bits. This preserves their respawn and fixed-slot semantics. */
    M68KState saved = g_cpu;
    int camera = r16(g_ram,0xB1D2), top = r16(g_ram,0xB1D6);
    int end = stage_width(g_ram);
    int span = s_requested_width < end ? s_requested_width : end;
    int left = view_left(camera,s_requested_width,g_ram)-32;
    int right = left+32+span+32;
    if (left < 0) left=0;
    if (right > end) right=end;
    unsigned shift=read16(g_ram,0x116C2+r16(g_ram,0xB1D0)*2);
    for (int bottom=0;bottom<2;++bottom) {
        /* Cells start strictly before the right edge, as in the original
         * loader. A cell at stage_width would index past the placement map. */
        for (int x=left&~255;x<right;x+=256) {
            g_cpu.D[2]=(uint16_t)left;g_cpu.D[4]=(uint16_t)right;
            g_cpu.D[3]=(uint16_t)top;g_cpu.D[5]=(uint16_t)(top+256);
            g_cpu.D[6]=(uint16_t)x;g_cpu.D[7]=(uint16_t)(top+(bottom?256:0));
            g_cpu.A[4]=shift;
            recomp_call_addr(bottom?0x11548:0x1150C);
        }
    }
    g_cpu=saved;
}
int rka_video_hook(uint32_t pc)
{
    if (!enabled()) return 0;
    if (pc == 0x69BE || pc == 0x6AD2 || pc == 0x6BDA ||
        pc == 0x7964 || pc == 0x803A) { capture_scroll(); return 0; }
    if (pc == 0x1FB0) {
        s_build.count = 0;
        /* The actor camera includes shake and the SAT's 128px bias. */
        s_build.camera_x = (uint16_t)(r16(g_ram, 0xB1C8) + 128);
        s_build.camera_y = (uint16_t)(r16(g_ram, 0xB1CC) + 128);
        s_build.stage = r16(g_ram, 0xFB12);
    } else if (pc == 0x21AE) capture_actor();
    else if (pc == 0x2358) {
        memcpy(s_build.ram, g_ram, sizeof s_build.ram);
        memcpy(s_build.sat, g_ram + 0xF780, sizeof s_build.sat);
        s_build.serial = ++s_serial;
        s_history[s_serial % 3] = s_build;
    }
    if (!s_expanded || s_requested_width <= 320) return 0;
    if (pc == 0x113D8 && scene_active(g_ram)) { spawn_visible();return 1; }
    if (pc == 0x115EA) ++s_spawned;
    /* Vehicle and other map-mode-1 scenes have no tiled placement map, but
     * they still use the normal object culling path. Keep their live actors
     * across the expanded view instead of deleting them at the 4:3 edge. */
    unsigned map = r16(g_ram,0xB1D0);
    if (map < 1 || map > 9) return 0;
    int camera = r16(g_ram, 0xB1D2), left = view_left(camera, s_requested_width, g_ram);
    int end = stage_width(g_ram);
    int span = s_requested_width < end ? s_requested_width : end;
    int extra_left = camera - left, extra_right = left + span - camera - 320;
    if (extra_right < 0) extra_right = 0;
    /* The two vehicle sequences allocate at fixed native screen edges rather
     * than using the placement map. Shift only their newly written X words;
     * their own cadence, object pool, motion and despawn logic stay native. */
    unsigned stage = r16(g_ram,0xFB12);
    if (map == 1 &&
        ((stage == 2 && pc == 0x2F8FE) ||
         (stage == 7 && pc == 0x16E04))) {
        unsigned slot = g_cpu.A[2] & 65535u;
        int x = (int16_t)r16(g_ram,slot+6);
        int shift = pc == 0x2F8FE ? extra_right :
                    (x == 0 ? -extra_left : extra_right);
        if (shift) m68k_write16(0xFF0000u+slot+6,(uint16_t)(x+shift));
        return 0;
    }
    unsigned o = g_cpu.A[1] & 65535u;
    /* Pre-branch hooks replace only the preceding X comparison flags. The
     * original branch, respawn bookkeeping and deletion routines still run. */
    if (!(g_ram[(o + 0x27) & 65535u] & 4)) {
        if (pc == 0x20AE || pc == 0x2128) compare_word((int16_t)r16(g_ram,o+0x28), 0x220 + extra_right);
        if (pc == 0x20BE || pc == 0x2138) compare_word((int16_t)r16(g_ram,o+0x28), 0x20 - extra_left);
    }
    return 0;
}

static void select_scene(const GVDP *v)
{
    unsigned sat = (v->reg[5] & 0x7Eu) << 9;
    const Scene *best = NULL;
    for (unsigned i = 0; i < 3; ++i)
        if (s_history[i].serial && !memcmp(s_history[i].sat, v->vram + sat, 640) &&
            (!best || s_history[i].serial > best->serial)) best = &s_history[i];
    if (best) { s_presented = *best; s_scene = &s_presented; }
    else if (s_scene && (s_scene->stage != (int)r16(g_ram,0xFB12) ||
                        memcmp(s_scene->sat,v->vram+sat,640))) s_scene = NULL;
}
static void draw_sprite(const GVDP *v, const Sprite *s, int line, uint32_t *out,
                        int width, int origin, int camera_x, int camera_y,
                        const uint32_t *palette)
{
    int x = s->screen ? s->x + (width - 320) / 2 : s->x - camera_x + origin;
    if (s->castle_parallax && s_scene && origin != (width - 320) / 2) {
        int64_t camera = (int32_t)r32(s_scene->ram,0xB1D2) + (int64_t)160 * 65536;
        /* At a clamped edge the wider camera is on an integer pixel. Do not
         * let the moving native camera's fractional part move the scenery. */
        int64_t wide = (camera >> 16) * 65536 +
                       (int64_t)((width - 320) / 2 - origin) * 65536;
        int64_t change = (wide - (wide >> 3)) - (camera - (camera >> 3));
        x += (int)((s->fraction_x + change) >> 16);
    }
    int y = s->screen ? s->y : s->y - camera_y;
    int wt = ((s->size >> 2) & 3) + 1, ht = (s->size & 3) + 1;
    int dy = line - y;
    if (dy < 0 || dy >= ht * 8) return;
    int ty = (s->attr & 0x1000) ? ht - 1 - dy / 8 : dy / 8;
    for (int dx = 0; dx < wt * 8; ++dx) {
        int dst = x + dx;
        if (dst < 0 || dst >= width || s_occupied[dst]) continue;
        int tx = (s->attr & 0x0800) ? wt - 1 - dx / 8 : dx / 8;
        unsigned attr = (s->attr & ~2047u) | ((s->attr + tx * ht + ty) & 2047u);
        unsigned p = pixel(v, attr, dx, dy);
        if (!p) continue;
        s_occupied[dst] = 1;
        if ((s->attr & 0x8000) || !s_priority[dst]) out[dst] = palette[p];
    }
}

static void draw_hud_margin(const uint32_t *native, uint32_t *out, int width)
{
    int center = (width - 320) / 2;
    /* Repeat the undecorated left border of the live 32px window across both
     * margins. Its four tiles carry the cornice and opaque side panel but no
     * counters or hearts; water cannot show through the lower HUD rows. */
    for (int x=0;x<width;++x) {
        if (x >= center && x < center + 320) continue;
        out[x] = native[(unsigned)(x-center) & 31u];
    }
}
static void scanline(const GVDP *v, int line, const uint32_t *native, int nw,
                     uint32_t *out, int width)
{
    uint32_t palette[64];
    for (int i = 0; i < 64; ++i) palette[i] = genesis_dac_cram_to_argb(v->cram[i], GENESIS_DAC_NORMAL);
    uint32_t backdrop = palette[v->reg[7] & 63];
    if (!line) {
        ++s_frames; s_width = s_requested_width = width;
        s_terrain_checks = s_terrain_errors = 0;
        if (width > s_capacity) {
            uint8_t *a = malloc((size_t)width), *b = malloc((size_t)width);
            if (a && b) { free(s_priority); free(s_occupied); s_priority=a; s_occupied=b; s_capacity=width; }
            else { free(a); free(b); }
        }
        select_scene(v);
    }
    const uint8_t *r = s_scene ? s_scene->ram : g_ram;
    int level = (scene_active(r) && scene_active(g_ram)) ||
                (vehicle_active(r) && vehicle_active(g_ram));
    if (nw != 320 || !(v->reg[1] & 64) || (v->reg[12] & 14) || s_capacity < width) {
        for (int x = 0; x < width; ++x) out[x] = !(v->reg[1] & 64) ? native[0] : backdrop;
        memcpy(out + (width - nw) / 2, native, (size_t)nw * sizeof *out);
        return;
    }
    if (!level) {
        for (int x = 0; x < width; ++x) out[x] = backdrop;
        memcpy(out + (width - nw) / 2, native, (size_t)nw * sizeof *out);
        return;
    }
    unsigned hs = (v->reg[13] & 63u) << 10;
    int hm = v->reg[11] & 3, hi = hm == 0 ? 0 : hm == 2 ? line & ~7 : line;
    int ha = (int16_t)r16(v->vram, hs + hi * 4);
    /* RKA fills the name-table rows hidden by its HUD with an opaque black
     * tile. Extend the first visible background tile row upward into those
     * newly exposed margins, using its live art, palette and scroll strip. */
    int bg_line = level && v->reg[18] == 4 && line < 32 ? 32 + (line & 7) : line;
    int bhi = hm == 0 ? 0 : hm == 2 ? bg_line & ~7 : bg_line;
    int hb = (int16_t)r16(v->vram, hs + bhi * 4 + 2);
    int camera = unwrap(-ha, r16(r, 0xB1D2));
    int cy = unwrap(v->vsram[0], r16(r, 0xB1D6));
    if (!line) { s_camera = camera; s_left = level ? view_left(camera,width,r) : camera-(width-nw)/2; }
    /* Keep the camera anchor fixed across the frame so per-strip/per-line
     * horizontal scroll remains visible instead of cancelling itself out. */
    int origin = level ? s_camera - s_left : (width - nw) / 2;
    int center = (width - nw) / 2;
    unsigned map = r16(r,0xB1D0), stage = r16(r,0xFB12);
    int bg_h = level && hm == 2 ? background_scroll(r,bg_line,hb,center-origin) : hb;
    if (line == 128) s_bg_x = -center - bg_h;
    s_origin = origin;
    unsigned ba = (v->reg[2] & 56u) << 10, bb = (v->reg[4] & 7u) << 13;
    memset(s_occupied,0,(size_t)width);
    for (int x = 0; x < width; ++x) {
        int nx = x - origin, wx = nx + camera;
        int col = 0;
        if (v->reg[11] & 4) {
            int xc = nx < 0 ? 0 : nx >= nw ? nw - 1 : nx;
            col = (xc >> 4) * 2;
        }
        int va = v->vsram[col] & 1023, vb = v->vsram[col+1] & 1023;
        int wy = unwrap(va,r16(r,0xB1D6)) + line;
        int bx = x - center - bg_h, by = bg_line + vb;
        int edge_x = nx < 0 ? (nx & 31) : nx >= nw ? nw-32+((nx-nw)&31) : nx;
        /* The water ride streams plane A through the native window. Reuse a
         * known-good 32px sky strip for its newly exposed side columns. */
        int vehicle_sky = level && map == 1 && stage == 2 &&
                          (nx < 0 || nx >= nw);
        if (vehicle_sky) bx = 128 + (nx & 31) + origin - center - bg_h;
        /* Non-layout scenes stream only the native 320px plane A window.
         * Beyond it, the wrapped name table can contain stale boss tiles or
         * the previous room. Let the live plane B scenery fill that space. */
        unsigned a = level && map != 1 ? terrain(r,wx,wy) :
                     vehicle_sky ? plane(v,ba,128+(nx&31)-ha,line+va) :
                     level && map == 1 ? plane(v,ba,edge_x-ha,line+va) :
                     nx >= 0 && nx < nw ? plane(v,ba,nx-ha,line+va) : 0;
        unsigned b = plane(v,bb,bx,by);
        unsigned ap = bg_line == line ? pixel(v,a,wx,wy) : 0;
        unsigned bp = pixel(v,b,bx,by);
        unsigned color = ap && (a & 0x8000) ? ap : bp && (b & 0x8000) ? bp : ap ? ap : bp;
        out[x] = color ? palette[color] : backdrop;
        s_priority[x] = (uint8_t)((ap && (a & 0x8000)) || (bp && (b & 0x8000)));
        if (level && nx >= 0 && nx < nw && !(x & 7)) {
            unsigned expected = plane(v,ba,nx-ha,line+va);
            ++s_terrain_checks;
            if (pixel(v,a,wx,wy) != pixel(v,expected,wx,wy)) ++s_terrain_errors;
        }
    }
    if (stage == 23 && map == 1 && width > 512) {
        /* The flying-city set piece has only a 256px repeating background,
         * even though its camera has no finite level limit. Across a very
         * wide view, five identical skylines strobe behind the player. Keep
         * the native view intact, then dissolve the repeats into a smoothed
         * average of the city's colors before drawing the moving actors. */
        static unsigned fog_red, fog_green, fog_blue;
        unsigned red=0, green=0, blue=0;
        int sample=origin < 0 ? 0 : origin;
        int count=width-sample < 256 ? width-sample : 256;
        for (int x=sample;x<sample+count;++x) {
            red += (out[x] >> 16) & 255u;
            green += (out[x] >> 8) & 255u;
            blue += out[x] & 255u;
        }
        if (count > 0) {
            red /= (unsigned)count; green /= (unsigned)count; blue /= (unsigned)count;
            if (line <= 32) { fog_red=red; fog_green=green; fog_blue=blue; }
            else {
                fog_red=(fog_red*23u+red)/24u;
                fog_green=(fog_green*23u+green)/24u;
                fog_blue=(fog_blue*23u+blue)/24u;
            }
            for (int x=0;x<width;++x) {
                int distance=x<origin ? origin-x : x>=origin+nw ? x-origin-nw+1 : 0;
                if (!distance) continue;
                unsigned mix=distance < 128 ? (unsigned)distance*2u : 256u;
                uint32_t old=out[x];
                unsigned r=((old>>16&255u)*(256u-mix)+fog_red*mix)>>8;
                unsigned g=((old>>8&255u)*(256u-mix)+fog_green*mix)>>8;
                unsigned b=((old&255u)*(256u-mix)+fog_blue*mix)>>8;
                out[x]=0xFF000000u | r<<16 | g<<8 | b;
                if (mix >= 128) s_priority[x]=0;
            }
        }
    }
    /* Player/HUD pieces occupy the reserved SAT prefix; actor mappings
     * captured above replace the remaining SAT, including offscreen pieces. */
    unsigned sat = (v->reg[5] & 0x7Eu) << 9, index = 0;
    int on_line = 0;
    if (s_scene)
        for (unsigned i=0;i<s_scene->count;++i)
            if(s_scene->sprites[i].front)
                draw_sprite(v,&s_scene->sprites[i],line,out,width,origin,
                            s_scene->camera_x,s_scene->camera_y,palette);
    for (int i=0;i<80;++i) {
        unsigned addr=sat+index*8;
        unsigned raw_x=r16(v->vram,addr+6)&511;
        int sy=(int)(r16(v->vram,addr)&511)-128;
        int sh=((v->vram[(addr+2)&65535u]&3)+1)*8;
        if (line>=sy && line<sy+sh) {
            ++on_line;
            if (!raw_x && on_line>1) break;
        }
        /* RKA's first SAT entry is a deliberately offscreen masking sentinel
         * at (-120,0), followed by its X=0 partner. A wider canvas must not
         * reveal its arbitrary tile-zero graphics beside the HUD. */
        int sentinel = index == 0 && raw_x == 8 && sy == 0 && sh == 32 &&
                       (r16(v->vram,sat+14)&511) == 0;
        if (raw_x && !sentinel && (!s_scene || index<10)) {
            Sprite s = {0};
            s.x=(int)(r16(v->vram,addr+6)&511)-128+camera;
            s.y=(int)(r16(v->vram,addr)&511)-128+cy;
            s.size=v->vram[(addr+2)&65535u];s.attr=(uint16_t)r16(v->vram,addr+4);s.screen=0;s.front=0;
            draw_sprite(v,&s,line,out,width,origin,camera,cy,palette);
        }
        index=v->vram[(addr+3)&65535u]&127u;
        if (!index || index>=80) break;
    }
    if (s_scene)
        for (unsigned i=0;i<s_scene->count;++i)
            if(!s_scene->sprites[i].front)
                draw_sprite(v,&s_scene->sprites[i],line,out,width,origin,
                            s_scene->camera_x,s_scene->camera_y,palette);
    /* Preserve the hardware-rendered original viewport exactly. Raster
     * effects, special boss sprites and streamed tile changes can differ
     * from our reconstruction; only the added side pixels need rebuilding. */
    int window_y=(v->reg[18]&31)*8;
    int window_line=(v->reg[18]&128) ? line>=window_y : line<window_y;
    if (!window_line && (origin == center || (stage == 23 && map == 1)))
        memcpy(out+origin,native,(size_t)nw*sizeof *out);
    /* Several room transitions keep the previous room's layout entries in
     * the extra columns even though the native edge is a solid room wall.
     * Continue the live 32px wall strip at those audited stage boundaries. */
    if (!window_line && level) {
        int wall_left = stage == 4 || stage == 11 || stage == 15 || stage == 41;
        int wall_right = stage == 6 || stage == 40 || stage == 15;
        if (wall_left)
            for (int x=0;x<origin && x<width;++x)
                out[x] = native[(unsigned)(x-origin) & (stage == 15 ? 7u : 31u)];
        if (wall_right)
            for (int x=origin+nw;x<width;++x)
                out[x] = native[nw-(stage == 15 ? 8 : 32)+
                                ((x-origin-nw)&(stage == 15 ? 7 : 31))];
        /* The rail is a native strip effect, not a level-map tile. */
        if (stage == 7 && map == 1 && line >= 214)
            for (int x=0;x<width;++x)
                if (x < origin || x >= origin+nw)
                    out[x] = native[16+((unsigned)x&15u)];
    }
    /* The native window is a screen-space HUD, never a repeated world map. */
    if (window_line) {
        int start=(width-nw)/2;
        if (window_y == 32 && !(v->reg[18]&128))
            draw_hud_margin(native,out,width);
        memcpy(out+start,native,(size_t)nw*sizeof *out);
    }
}
void rka_video_command(int id, const char *json)
{
    (void)json;
    char out[640];
    snprintf(out,sizeof out,"{\"id\":%d,\"enabled\":%d,\"width\":%d,\"view_left\":%d,"
        "\"camera\":%d,\"native_x\":%d,\"bg_x_128\":%d,\"frames\":%u,\"terrain_checks\":%u,"
        "\"terrain_errors\":%u,\"sprites\":%u,\"spawn_mode\":\"%s\",\"spawned\":%u,\"sprite_overflow\":%u,\"published\":%u,\"displayed\":%u}",
        id,enabled(),s_width,s_left,s_camera,s_origin,s_bg_x,s_frames,s_terrain_checks,s_terrain_errors,
        s_scene?s_scene->count:0,rka_video_spawn_mode(),s_spawned,s_sprite_overflow,s_serial,s_scene?s_scene->serial:0);
    cmd_send_response(out);
}
const GameVideo rka_video = { configure, enabled, width, scanline };
