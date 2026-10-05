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
    uint16_t attr, fraction_x, object, type;
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
static unsigned s_sprite_overflow, s_edge_shifts;
static int s_width, s_left, s_origin, s_camera;
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
/* Scene classes. Tiled maps 2-8 stream plane A from the byte layout
 * ($2D7E). Map 9 stops streaming for a boss arena; most tiled stages keep
 * a valid layout, but stage 1 keeps the previous area's metadata while its
 * chamber is authored in the live plane. The ending's map 9 has no layout.
 * Map 1 (vehicles, shooters, set pieces, cutscenes) never streams:
 * its planes are loaded whole and wrap at the plane width, so the live name
 * tables are valid beyond 320px. Every playable scene loads a block
 * dictionary at $B1F6; the Continue screen and stage cards do not, and keep
 * their centered presentation. */
enum { SCENE_NONE, SCENE_TILED, SCENE_PLANAR };
static int scene_kind(const uint8_t *r)
{
    unsigned mode = r16(r, 0xB002), map = r16(r, 0xB1D0);
    if ((mode != 4 && mode != 7) || !r32(r, 0xB1F6)) return SCENE_NONE;
    /* The earlier water tower is painted into the live plane. The tank
     * encounter at the stage's final camera still has a valid tiled bridge. */
    if ((map == 9 && r16(r, 0xFB12) == 2 &&
         r16(r, 0xB1D2) != r16(r, 0xB1E8)) ||
        (map == 3 && r16(r, 0xFB12) == 21)) return SCENE_PLANAR;
    if (map >= 2 && map <= 8) return r32(r, 0xB1FA) ? SCENE_TILED : SCENE_NONE;
    /* Stage 1's boss keeps the preceding room's layout metadata while its
     * chamber and boss are drawn into the live name table. Decoding that
     * stale layout replaces the encounter with outdoor sky. */
    if (map == 9 && r16(r, 0xFB12) == 11) return SCENE_PLANAR;
    if (map == 9 && r32(r, 0xB1FA) && r16(r, 0xB1E4) != 0xFFFF) return SCENE_TILED;
    return map == 1 || map == 9 ? SCENE_PLANAR : SCENE_NONE;
}
/* $B1E8 (and the room limit $B39E) is the last camera X, one less than the
 * layout width minus 320: a 4096px map stops its camera at 3775, so its
 * final pixel column is never on screen natively. */
static int stage_width(const uint8_t *r) { return (int)r16(r, 0xB1E8) + 321; }
/* $3EAE clamps the following camera to [$B39C,$B39E], the current room.
 * Its scripted pans ($3C50) may move outside a stale range, so the bounds
 * always include the live camera. Returns the room's world span [lo,hi). */
static void room_span(const uint8_t *r, int camera, int *lo, int *hi)
{
    int stage = stage_width(r);
    *lo = (int16_t)r16(r, 0xB39C); *hi = (int16_t)r16(r, 0xB39E) + 320;
    if (*lo < 0 || *hi < *lo + 320 || *hi > stage) { *lo = 0; *hi = stage; }
    if (camera < *lo) *lo = camera;
    if (camera + 320 > *hi) *hi = camera + 320;
}
/* A room narrower than the view is centered on its midpoint, even at a
 * stage end, so a locked boss room keeps the exact native view (and any
 * plane-drawn boss) in the center; room_x fills both sides. A wider room
 * clamps the view inside it. Planar scenes have no layout bounds and are
 * always centered. */
static int view_left(int camera, int width, const uint8_t *r)
{
    if (scene_kind(r) != SCENE_TILED) return camera - (width - 320) / 2;
    int lo, hi;
    room_span(r, camera, &lo, &hi);
    if (hi - lo < width) return (lo + hi - width) / 2;
    int left = camera - (width - 320) / 2;
    if (left < lo) left = lo;
    if (left > hi - width) left = hi - width;
    return left;
}
/* Beyond a room the layout holds unrelated areas: the previous corridor,
 * the castle exterior beside an interior, the next section. Continue the
 * room's outermost whole 32px block column instead. Only terrain is
 * substituted; plane B and actors keep their real positions. */
/* The room's own 32px block columns. Camera limits are pixel-exact and
 * off by one ($B39E is a last camera X), so a room edge can expose one
 * pixel of the neighbouring block or hide one of its own: a block belongs
 * to the room when most of it is inside. Margins continue those blocks. */
static void room_blocks(int lo, int hi, int *first, int *last)
{ *first = (lo + 16) & ~31; *last = (hi + 16) & ~31; }
/* What the native view shows of the room: blocks at least half inside. The
 * lava arena and machine room start exactly 16px into a block; that half is
 * on screen natively, with its actors (a platform piece), so it keeps its
 * native pixels while the margins still continue the next whole block. */
static void room_cover(int lo, int hi, int *first, int *last)
{ *first = (lo + 15) & ~31; *last = (hi + 16) & ~31; }
static int room_x(int wx, int lo, int hi)
{
    int first, last;
    room_blocks(lo, hi, &first, &last);
    if (last - first < 32) return wx;
    if (wx < first) return first + ((wx - first) & 31);
    if (wx >= last) return last - 32 + ((wx - last) & 31);
    return wx;
}
static int bridge_x(int wx, int lo, int hi)
{
    room_blocks(lo, hi, &lo, &hi);
    /* The stage-1 tank arena is exactly one native viewport wide. Repeating
     * either sloped 32px edge block makes the suspension cable sawtooth at
     * wide ratios. Reflect the authored bridge span at each end instead;
     * the rail and posts meet the native viewport without a height jump. */
    int span = hi - lo;
    if (span <= 0) return wx;
    int n = (wx - lo) % (span * 2);
    if (n < 0) n += span * 2;
    return lo + (n < span ? n : span * 2 - 1 - n);
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
static void wave_reset(void);
void rka_video_reset(void)
{
    memset(&s_build, 0, sizeof s_build);
    memset(s_history, 0, sizeof s_history);
    memset(&s_presented, 0, sizeof s_presented);
    s_scene = NULL; s_serial = s_frames = s_spawned = s_edge_shifts = 0;
    s_sprite_overflow = s_terrain_checks = s_terrain_errors = 0;
    memset(s_scroll,0,sizeof s_scroll); s_scroll_serial = 0;
    wave_reset();
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

/* Per-column vertical scroll generator $7DBC (the stage-6 chain sway and
 * its plane-B twin): for column k it stores
 *   -(sin8(angle + (k+1)*step) >> shift + $B1D6 + a5) + a4
 * (B clamps at -720) into the VSRAM shadow, world-anchored, 32 columns of
 * which the VDP shows 20. The loop head $7DFC records the parameters so the
 * margins continue the same wave instead of freezing the edge column. */
typedef struct {
    unsigned serial;
    uint16_t angle, step, camera_y, a5, a4;
    uint8_t shift, clamp;
} Wave;
enum { WAVE_HISTORY = 4 };
static Wave s_wave[2][WAVE_HISTORY];
static unsigned s_wave_serial;
static const Wave *s_wave_sel[2];
static unsigned s_wave_hits, s_wave_misses;
static void wave_reset(void)
{
    memset(s_wave, 0, sizeof s_wave); s_wave_serial = s_wave_hits = s_wave_misses = 0;
    s_wave_sel[0] = s_wave_sel[1] = NULL;
}
static void capture_wave(void)
{
    if ((g_cpu.D[2] & 0xFFFF) != 0x1F) return;      /* first column only */
    unsigned base = g_cpu.A[3] & 0xFFFF;
    int p = base == 0xF600 ? 0 : base == 0xF602 ? 1 : -1;
    if (p < 0) return;
    Wave *w = &s_wave[p][++s_wave_serial % WAVE_HISTORY];
    w->serial = s_wave_serial;
    w->angle = (uint16_t)g_cpu.D[1]; w->step = (uint16_t)g_cpu.D[6];
    w->shift = (uint8_t)(g_cpu.D[0] & 15); w->camera_y = (uint16_t)r16(g_ram, 0xB1D6);
    w->a5 = (uint16_t)g_cpu.A[5]; w->a4 = (uint16_t)g_cpu.A[4];
    w->clamp = r16(g_ram, 0x2818) != 0;
}
static unsigned wave_value(const Wave *w, int k)
{
    unsigned angle = (unsigned)(w->angle + (k + 1) * w->step) & 0xFF;
    int sine = (int8_t)g_rom[0xF28 + angle] >> w->shift;
    int16_t v = (int16_t)(-(sine + w->camera_y + (int16_t)w->a5) + (int16_t)w->a4);
    if (w->clamp && v >= -720) v = -720;
    return (unsigned)v & 1023u;
}
/* The newest recorded wave that reproduces all 20 live VSRAM columns. */
static void select_waves(const GVDP *v)
{
    for (int p = 0; p < 2; ++p) {
        s_wave_sel[p] = NULL;
        if (!(v->reg[11] & 4)) continue;
        for (int i = 0; i < WAVE_HISTORY; ++i) {
            const Wave *w = &s_wave[p][i];
            if (!w->serial || (s_wave_sel[p] && s_wave_sel[p]->serial > w->serial)) continue;
            int k = 0;
            while (k < 20 && wave_value(w, k) == (v->vsram[k * 2 + p] & 1023u)) ++k;
            if (k == 20) s_wave_sel[p] = w;
        }
        if (s_wave_sel[p]) ++s_wave_hits; else if (s_wave[p][0].serial || s_wave[p][1].serial) ++s_wave_misses;
    }
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
            /* Signed: vehicle and shooter scenes enter actors from the
             * left at negative X, alive (and firing) before they reach 0. */
            x += (int16_t)r16(g_ram,o+6); y += (int16_t)r16(g_ram,o+10);
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
            s->object = (uint16_t)o; s->type = (uint16_t)type;
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
    int view = view_left(camera,s_requested_width,g_ram), lo, hi;
    /* Widen only across the room the view really shows. Margins beyond a
     * locked room are continued wall, not a reason to wake the neighbouring
     * area; the native camera-32..camera+352 range always remains. */
    room_span(g_ram,camera,&lo,&hi);
    int left = (view > lo ? view : lo) - 32;
    int right = (view+s_requested_width < hi ? view+s_requested_width : hi) + 32;
    /* The fire corridor's linked heat controller sits just beyond the
     * native right edge in the supplied save. Waking it from either added
     * margin starts its launcher early and leaves a stack of fire sprites
     * in the native view. Keep this one room on its original spawn cadence. */
    if (r16(g_ram,0xFB12) == 3 && r16(g_ram,0xB1D0) == 5) {
        left = camera - 32;
        right = camera + 352;
    }
    if (left > camera-32) left = camera-32;
    if (right < camera+352) right = camera+352;
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
/* Always-on spawn ring. $116B0 clears every new object slot; its return
 * address is the creation site. The slot's type and position are filled at
 * the next actor snapshot ($2358), after the creator has stored them.
 * rka_spawns reads the ring; recording never touches guest state. */
typedef struct {
    uint32_t serial, frame, caller, writer[2];
    uint16_t slot, type, camera;
    int16_t x, screen_x, shift;
    uint8_t map, stage, filled, flags;
} Spawn;
enum { SPAWN_RING = 4096 };
static Spawn s_spawns[SPAWN_RING];
static unsigned s_spawn_count, s_spawn_filled;
/* Guest stack words straight from work RAM: no bus access, no cycles. */
static uint32_t stack32(uint32_t a)
{ return (r16(g_ram,a & 65535u) << 16 | r16(g_ram,(a + 2) & 65535u)) & 0xFFFFFFu; }
/* Creator attribution for slots filled without $116B0: the engine's write
 * hook sees every guest write; keep the caller chain of the latest write to
 * each slot's type word. Installed whenever no debug write log owns it. */
extern void (*g_mem_write_trace_fn)(uint32_t byte_address, uint8_t value, uint32_t target_cycle);
static uint32_t s_type_writer[64][2];
static void watch_writes(uint32_t address, uint8_t value, uint32_t cycle)
{
    (void)value; (void)cycle;
    address &= 0xFFFFFFu;
    if (address < 0xFFC100u || address >= 0xFFE000u || (address & 0x7Eu)) return;
    unsigned i = (address - 0xFFC000u) >> 7;
    s_type_writer[i][0] = stack32(g_cpu.A[7]);
    /* Inside the object loop ($201A) A1 is the running creator object. */
    s_type_writer[i][1] = (r16(g_ram,g_cpu.A[1] & 65535u) << 16) | (g_cpu.A[1] & 65535u);
}
static void record_spawn(void)
{
    unsigned slot = g_cpu.A[2] & 0xFFFFFFu;
    if (slot < 0xFFC000u || slot >= 0xFFE000u) return;
    Spawn *e = &s_spawns[s_spawn_count % SPAWN_RING];
    memset(e, 0, sizeof *e);
    e->serial = s_spawn_count++;
    e->frame = cmd_server_current_frame();
    e->caller = stack32(g_cpu.A[7]);
    e->writer[0] = e->caller;
    e->writer[1] = (r16(g_ram,g_cpu.A[1] & 65535u) << 16) | (g_cpu.A[1] & 65535u);
    e->slot = (uint16_t)slot;
}
static uint16_t s_slot_type[64];
static void describe_spawn(Spawn *e)
{
    unsigned o = e->slot;
    e->type = (uint16_t)r16(g_ram,o); e->x = (int16_t)r16(g_ram,o+6);
    e->camera = (uint16_t)r16(g_ram,0xB1D2);
    e->map = (uint8_t)r16(g_ram,0xB1D0); e->stage = (uint8_t)r16(g_ram,0xFB12);
    /* $28 is the culling screen X + 128 once the object has run; before
     * that, map-1 objects are screen-space and others world-space. */
    e->screen_x = r16(g_ram,o+0x28) ? (int16_t)(r16(g_ram,o+0x28) - 128) :
                  (int16_t)(e->map == 1 ? e->x : e->x - (int16_t)e->camera);
    e->flags = g_ram[(o + 0x27) & 65535u];
    e->filled = 1;
}
static void fill_spawns(void)
{
    uint64_t attributed = 0;
    if (s_spawn_count - s_spawn_filled > SPAWN_RING) s_spawn_filled = s_spawn_count - SPAWN_RING;
    for (; s_spawn_filled < s_spawn_count; ++s_spawn_filled) {
        Spawn *e = &s_spawns[s_spawn_filled % SPAWN_RING];
        describe_spawn(e);
        attributed |= 1ull << ((e->slot - 0xC000u) / 0x80u);
    }
    /* Some generators fill slots without $116B0 (the shooter formations).
     * A slot whose type changed since the last snapshot is a birth too; its
     * caller stays 0 (observed, not attributed). */
    for (unsigned i = 2; i < 64; ++i) {
        unsigned o = 0xC000u + i * 0x80u, type = r16(g_ram,o);
        if (type && type != s_slot_type[i] && !(attributed >> i & 1)) {
            Spawn *e = &s_spawns[s_spawn_count % SPAWN_RING];
            memset(e, 0, sizeof *e);
            e->serial = s_spawn_count++;
            e->frame = cmd_server_current_frame();
            e->slot = (uint16_t)(0xFF0000u + o);
            e->writer[0] = s_type_writer[i][0]; e->writer[1] = s_type_writer[i][1];
            describe_spawn(e);
            ++s_spawn_filled;
        }
        s_slot_type[i] = (uint16_t)type;
    }
}
void rka_spawns_command(int id, const char *json)
{
    /* {"since":N} returns up to 256 entries with serial >= N. */
    unsigned since = 0;
    const char *at = json ? strstr(json, "\"since\"") : NULL;
    if (at && (at = strchr(at, ':'))) since = (unsigned)strtoul(at + 1, NULL, 10);
    unsigned oldest = s_spawn_count > SPAWN_RING ? s_spawn_count - SPAWN_RING : 0;
    if (since < oldest) since = oldest;
    static char out[256 * 200 + 128];
    int n = snprintf(out, sizeof out, "{\"id\":%d,\"count\":%u,\"spawns\":[", id, s_spawn_count);
    for (unsigned i = since, k = 0; i < s_spawn_count && k < 256 &&
         n < (int)sizeof out - 512; ++i, ++k) {
        const Spawn *e = &s_spawns[i % SPAWN_RING];
        n += snprintf(out + n, sizeof out - (size_t)n,
            "%s{\"serial\":%u,\"frame\":%u,\"caller\":\"%06X\",\"writer\":\"%06X\",\"creator\":\"%08X\",\"slot\":\"%04X\",\"type\":%u,"
            "\"x\":%d,\"screen_x\":%d,\"shift\":%d,\"camera\":%u,\"map\":%u,\"stage\":%u,\"flags\":%u,\"filled\":%u}",
            k ? "," : "", e->serial, e->frame, e->caller, e->writer[0], e->writer[1], e->slot, e->type, e->x, e->screen_x,
            e->shift, e->camera, e->map, e->stage, e->flags, e->filled);
    }
    snprintf(out + n, sizeof out - (size_t)n, "]}");
    cmd_send_response(out);
}
/* Fixed-edge creation sites from tools/audit_edge_spawns.py: the hook PC is
 * the instruction after the final X write into the new slot. camera: X is
 * camera-relative world space (tiled scenes); otherwise map-1 screen space.
 * zero_left: X=0 is a left-edge entry ($9A, and the rail table's 0/320). */
typedef struct { uint32_t pc; uint8_t camera, zero_left; uint16_t right_min; } EdgeSpawn;
static const EdgeSpawn s_edge_spawns[] = {
    /* The fire corridor's fixed launcher starts at camera+$130 (X=304),
     * sixteen pixels inside the native edge. Its new parts and projectiles
     * must follow the wider edge together, or they appear in mid-screen. */
    { 0x01378Cu, 1, 0, 304 }, /* type $3E launcher */
    { 0x013814u, 1, 0, 304 }, /* periodic type $3F fire source */
    { 0x013898u, 1, 0, 304 }, /* type $69 launcher phase */
    { 0x0138C0u, 1, 0, 304 }, /* second type $3F fire source */
    { 0x0138E8u, 1, 0 }, /* $0138DC move.w $b1d2.w,$6(a2) ; addi #$180 */
    { 0x016E04u, 0, 1 }, /* $016DFE type $96 X from the 0/320 table $16E46 */
    { 0x016E38u, 0, 1 }, /* $016E34 type $9A clr.w $6(a2), enters at +1px */
    { 0x01986Au, 1, 0 }, /* $01985E type $F6 at camera+$160 */
    { 0x020ED6u, 1, 0 }, /* $020EC4 type $169 at camera-$20+$180 */
    /* Register-passed X: D0 = camera+$140 handed to the particle builder
     * $3689E/$368B6 (fire-corridor embers drifting in from the right). */
    { 0x03027Cu, 1, 0 }, /* $030276 jsr $3689E, type $55 at camera+$140 */
    { 0x0303E2u, 1, 0 }, /* $0303DC jsr $3689E, type $55 at camera+$140 */
    { 0x02ED8Au, 0, 0 }, /* $02ED84 X=-48 */
    { 0x02EDB2u, 0, 0 }, /* $02EDAC X=-40 */
    { 0x02F8FEu, 0, 0 }, /* $02F8F6 water-ride type $11 at X=$160 */
    { 0x03077Eu, 1, 0 }, /* $03077A type $16 at camera+$140 */
    { 0x031CD2u, 0, 0 }, /* $031CCA X=-80 */
    { 0x036C74u, 0, 0 }, /* $036C6E X=-64 */
    { 0x0390D0u, 0, 0 }, /* $0390C8 X=$160 */
    { 0x03910Eu, 0, 0 }, /* $039106 X=$160 */
    { 0x0391A0u, 0, 0 }, /* $03919A X=$160 */
    { 0x03B5E0u, 0, 0 }, /* $03B5D8 X=$150 */
    { 0x03C396u, 0, 0 }, /* $03C392 shooter wave $BB from $BC at X=$150 */
    { 0x03CF86u, 0, 0 }, /* $03CF7E X=$150 */
    { 0x03CFF2u, 0, 0 }, /* $03CFEA X=$150 */
    { 0x03D216u, 0, 0 }, /* $03D20E X=$150 */
    { 0x03D2A2u, 0, 0 }, /* $03D29A X=-16 */
};
static const EdgeSpawn *edge_spawn(uint32_t pc)
{
    for (unsigned i = 0; i < sizeof s_edge_spawns / sizeof *s_edge_spawns; ++i)
        if (s_edge_spawns[i].pc == pc) return &s_edge_spawns[i];
    return NULL;
}
int rka_video_hook(uint32_t pc)
{
    if (pc == 0x116B0) { record_spawn(); return 0; }
    if (pc == 0x2358) {
        if (!g_mem_write_trace_fn) g_mem_write_trace_fn = watch_writes;
        fill_spawns();
    }
    if (!enabled()) return 0;
    if (pc == 0x69BE || pc == 0x6AD2 || pc == 0x6BDA ||
        pc == 0x7964 || pc == 0x803A) { capture_scroll(); return 0; }
    if (pc == 0x7DFC) { capture_wave(); return 0; }
    if (pc == 0x1FB0) {
        s_build.count = 0;
        /* The actor camera includes shake and the SAT's 128px bias. */
        s_build.camera_x = (int16_t)(r16(g_ram, 0xB1C8) + 128);
        s_build.camera_y = (int16_t)(r16(g_ram, 0xB1CC) + 128);
        s_build.stage = r16(g_ram, 0xFB12);
    } else if (pc == 0x21AE) capture_actor();
    else if (pc == 0x2358) {
        memcpy(s_build.ram, g_ram, sizeof s_build.ram);
        memcpy(s_build.sat, g_ram + 0xF780, sizeof s_build.sat);
        s_build.serial = ++s_serial;
        s_history[s_serial % 3] = s_build;
    }
    if (!s_expanded || s_requested_width <= 320) return 0;
    if (pc == 0x113D8 && scene_kind(g_ram) == SCENE_TILED && r16(g_ram,0xB1D0) != 9) { spawn_visible();return 1; }
    if (pc == 0x115EA) ++s_spawned;
    if (pc == 0x21B4) {
        unsigned o = g_cpu.A[1] & 65535u;
        int x = (int16_t)r16(g_ram,o+0x28);
        /* Expanded culling keeps distant actors alive beyond the native
         * screen, but their hardware SAT X is only 9 bits. Any world-space
         * actor whose biased X is outside 0..511 can wrap broken pieces into
         * the original play area. Its unwrapped mapping was captured at
         * $21AE and is rendered at its true location in the wide view. */
        if (!(g_ram[o+0x27] & 4) && (x < 0 || x >= 512))
            g_cpu.SR &= (uint16_t)~SR_C;
        return 0;
    }
    /* Vehicle and other map-mode-1 scenes have no tiled placement map, but
     * they still use the normal object culling path. Keep their live actors
     * across the expanded view instead of deleting them at the 4:3 edge. */
    unsigned map = r16(g_ram,0xB1D0);
    if (map < 1 || map > 9) return 0;
    int camera = r16(g_ram, 0xB1D2), left = view_left(camera, s_requested_width, g_ram);
    int view_right = left + s_requested_width;
    if (scene_kind(g_ram) == SCENE_TILED) {
        /* As for placement: keep actors alive only across the shown room. */
        int lo, hi;
        room_span(g_ram, camera, &lo, &hi);
        if (left < lo) left = lo;
        if (view_right > hi) view_right = hi;
    }
    int extra_left = camera - left, extra_right = view_right - camera - 320;
    if (extra_left < 0) extra_left = 0;
    if (extra_right < 0) extra_right = 0;
    /* Scripted generators place new objects at fixed native screen edges
     * instead of using the placement map. Each audited site follows the X
     * store into a fresh slot (A2); move an off-view X to the matching wide
     * edge. Cadence, object pool, motion and despawn stay native. */
    const EdgeSpawn *edge = edge_spawn(pc);
    if (edge) {
        /* The fire corridor's launcher writes several interdependent X
         * positions. Shifting only the creation sites desynchronizes its
         * wave/ember drawing, so retain native placement in this room. */
        if (r16(g_ram,0xFB12) == 3 && map == 5) return 0;
        int kind = scene_kind(g_ram);
        if (edge->camera ? kind != SCENE_TILED : map != 1) return 0;
        unsigned slot = g_cpu.A[2] & 65535u;
        int x = (int16_t)r16(g_ram,slot+6), sx = edge->camera ? x - camera : x;
        int right_min = edge->right_min ? edge->right_min : 320;
        int shift = sx < 0 || (edge->zero_left && sx == 0) ? -extra_left : sx >= right_min ? extra_right : 0;
        if (shift) {
            g_ram[(slot+6) & 65535u] = (uint8_t)((x + shift) >> 8);
            g_ram[(slot+7) & 65535u] = (uint8_t)(x + shift);
            s_edge_shifts++;
        }
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
/* Per-line sprite layer, composited with the planes exactly as the VDP
 * does (genesis_vdp.c): first opaque sprite pixel wins, priority order
 * S-hi > A-hi > B-hi > S-lo > A-lo > B-lo > backdrop, and in
 * shadow/highlight mode palette-3 colours 14/15 are operators. */
static uint8_t *s_spr_idx, *s_spr_flags;
enum { SPR_OPAQUE = 1, SPR_HIGH = 2, SPR_SHADOW = 4, SPR_HILITE = 8 };
/* World X span of the shown room's own blocks. Beyond a locked room the
 * view continues its wall; actors that live out there (water surface
 * pieces just past a boss room, the next corridor's enemies) are not in the
 * room, so pieces entirely outside it are not drawn over that wall. */
static int s_actor_lo = INT_MIN, s_actor_hi = INT_MAX;
static void draw_sprite(const GVDP *v, const Sprite *s, int line, int width,
                        int origin, int camera_x, int camera_y, int sh_mode)
{
    if (!s->screen && (s->x + (((s->size >> 2) & 3) + 1) * 8 <= s_actor_lo || s->x >= s_actor_hi))
        return;
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
        if (dst < 0 || dst >= width || (s_spr_flags[dst] & SPR_OPAQUE)) continue;
        int tx = (s->attr & 0x0800) ? wt - 1 - dx / 8 : dx / 8;
        unsigned attr = (s->attr & ~2047u) | ((s->attr + tx * ht + ty) & 2047u);
        unsigned p = pixel(v, attr, dx, dy);
        if (!p) continue;
        if (sh_mode && p == 62) s_spr_flags[dst] |= SPR_HILITE;
        else if (sh_mode && p == 63) s_spr_flags[dst] |= SPR_SHADOW;
        else {
            s_spr_idx[dst] = (uint8_t)p;
            s_spr_flags[dst] |= SPR_OPAQUE | ((s->attr & 0x8000) ? SPR_HIGH : 0);
        }
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
static unsigned s_kind, s_lines_wide, s_lines_native;
static int s_room_lo, s_room_hi;
/* Always-on presentation ring: one record per rendered frame, so probes read
 * back the window of interest instead of arming a capture. Presentation-only
 * state; never touches the guest. Queried by custom_video_history. */
typedef struct {
    uint32_t frame, serial;
    int32_t camera, native_camera, actor_camera, view_left, origin, room_lo, room_hi;
    uint16_t stage, map, width, sprites;
    uint8_t kind, wrap_a, wrap_b, copied, native_lines, sparse_a, sparse_b;
} FrameRecord;
enum { FRAME_RING = 4096 };
static FrameRecord s_frame_ring[FRAME_RING];
static unsigned s_frame_ring_count;
static FrameRecord *frame_record(void)
{ return s_frame_ring_count ? &s_frame_ring[(s_frame_ring_count - 1) % FRAME_RING] : NULL; }
/* Per-frame name-table column survey: a column is blank when every row
 * selects a fully transparent pattern. Cutscenes author only the 320px they
 * show; such a plane's unseen columns are all blank. */
static uint8_t s_blank_tile[2048], s_plain_tile[2048], s_blank_col[2][128], s_bare_col[2][128];
static int s_sparse[2], s_sparse_lo[2], s_sparse_len[2];
static void survey_planes(const GVDP *v)
{
    for (unsigned t = 0; t < 2048; ++t) {
        const uint8_t *pattern = v->vram + t * 32u;
        unsigned any = 0, same = 1;
        for (unsigned i = 0; i < 32; ++i) {
            any |= pattern[i];
            same &= pattern[i] == pattern[0];
        }
        s_blank_tile[t] = !any;
        /* One solid colour: letterbox bars and flat fills, not artwork. */
        s_plain_tile[t] = same && (pattern[0] >> 4) == (pattern[0] & 15);
    }
    int wt = tiles(v->reg[16] & 3), ht = tiles((v->reg[16] >> 4) & 3);
    unsigned base[2] = { (v->reg[2] & 56u) << 10, (v->reg[4] & 7u) << 13 };
    for (int p = 0; p < 2; ++p)
        for (int c = 0; c < wt && c < 128; ++c) {
            int cells = 0, art = 0;
            for (int y = 0; y < ht; ++y) {
                unsigned t = r16(v->vram, base[p] + (unsigned)(y * wt + c) * 2u) & 2047u;
                cells += !s_blank_tile[t];
                art += !s_plain_tile[t];
            }
            s_blank_col[p][c] = (uint8_t)!cells;
            /* Bare: no artwork beyond solid fills (the ending's letterbox
             * bars run the full plane width) and at most a few stray cells. */
            s_bare_col[p][c] = (uint8_t)(art <= 3);
        }
    /* A plane whose authored columns leave at least 256 blank pixels is one
     * drawing (a boss, a beam, a ship) placed in screen space by H-scroll,
     * not a backdrop. The game writes that scroll as a signed screen offset
     * from the drawing's origin column; only the VDP wraps it at the plane
     * width. Keep the drawing as one span at that offset. */
    for (int p = 0; p < 2; ++p) {
        int best = 0, start = 0, n = wt < 128 ? wt : 128;
        for (int c = 0; c < n; ++c) {
            if (!s_blank_col[p][c] || (c && s_blank_col[p][c - 1])) continue;
            int len = 0;
            while (len < n && s_blank_col[p][(c + len) % n]) ++len;
            if (len > best) { best = len; start = c; }
        }
        s_sparse[p] = best * 8 >= 256 && best < n;
        if (!s_sparse[p]) continue;
        int first = (start + best) % n, span = (n - best) * 8, plane_w = n * 8;
        s_sparse_lo[p] = (first * 8 + span / 2) % plane_w < plane_w / 2 ? first * 8 : first * 8 - plane_w;
        s_sparse_len[p] = span;
    }
}
/* Unwrapped plane X to sample for a sparse plane, or INT_MIN outside its
 * drawing. A drawing that begins or ends exactly at a native screen edge
 * (a beam fired from off-screen) was authored to be cut there: its edge
 * column continues across that margin. scroll maps native screen X to plane
 * X (plane_x = screen_x - scroll). */
static int sparse_x(int p, int plane_x, int scroll, int nw)
{
    if (!s_sparse[p]) return plane_x;
    int lo = s_sparse_lo[p], hi = lo + s_sparse_len[p];
    if (plane_x >= lo && plane_x < hi) return plane_x;
    if (plane_x < lo && lo + scroll == 0) return lo + ((plane_x - lo) & 7);
    if (plane_x >= hi && hi + scroll == nw) return hi - 8 + ((plane_x - hi) & 7);
    return INT_MIN;
}
/* True when a plane is a finite picture: artwork fills the native view up to
 * both edge columns, and the columns the native view does not show include a run of at
 * least 32 artwork-free pixels (cutscenes author 320 pixels; the princess
 * room frames its picture with a doorway strip, then empty name table). Such
 * a plane continues its native image into the margins instead of showing
 * what lies beyond the picture. A plane blank at a native edge is a drawing
 * inside an empty layer (a plane-drawn boss, a beam) and is handled as a
 * sparse plane instead; periodic backdrops have no bare run. */
static int unauthored(const GVDP *v, int p, int scroll, int nw)
{
    int wt = tiles(v->reg[16] & 3), n = wt < 128 ? wt : 128, shown = (nw + 7) >> 3;
    int first = ((-scroll) >> 3) & (wt - 1) & 127;
    int last = ((nw - 1 - scroll) >> 3) & (wt - 1) & 127;
    if (s_bare_col[p][first] || s_bare_col[p][last] || shown >= n) return 0;
    /* The picture fills the native view: a sparse layer (the shooter's
     * asteroid field) can touch both edges by chance. */
    int art = 0;
    for (int k = 0; k < shown; ++k) art += !s_bare_col[p][(first + k) % n];
    if (art * 4 < shown * 3) return 0;
    int run = 0;
    for (int k = shown; k < n; ++k) {
        run = s_bare_col[p][(first + k) % n] ? run + 1 : 0;
        if (run >= 4) return 1;
    }
    return 0;
}
/* An authored 320px picture continues into the margins mirrored at its
 * edges: seamless at the native border, and no feature (a castle at one
 * edge) reappears whole on the opposite side. */
static int native_wrap(int nx, int nw)
{
    int n = ((nx % (2 * nw)) + 2 * nw) % (2 * nw);
    return n < nw ? n : 2 * nw - 1 - n;
}
/* The cannon cutscene's right-hand ground is plane art, so continue its
 * nearest edge tiles without repeating a flying actor at the screen edge. */
static int edge_plane_x(unsigned stage, unsigned map, int plane, int nx, int nw)
{
    if (plane == 0 && stage == 45 && map == 1 && nx >= nw)
        return nw - 1 - (nx - nw) % 32;
    /* These rooms end in solid masonry. Continue the plane's wall tiles,
     * never the composited native edge, which may include the player. */
    if (plane == 0 && stage == 11 && map == 9 && (nx < 0 || nx >= nw))
        return nx < 0 ? (-nx - 1) % 8 : nw - 1 - (nx - nw) % 8;
    return nx;
}
/* Large stage-set pieces are composed by the VDP itself, outside the normal
 * actor mapping capture. The live native edge contains their final pixels;
 * repeating a short edge strip carries the solid construction into a wide
 * margin without repeating an entire room or moving the set piece. */
static int native_edge_x(unsigned stage, unsigned map,
                         unsigned camera_x, int nx, int nw)
{
    int left = 0, right = 0, strip = 32;
    /* The earlier water tower needs its solid right edge carried outward.
     * At the final tank arena, that edge contains the moving boss and bridge;
     * its tile layout supplies the margins without reflecting either one. */
    if (stage == 2 && map == 9 && camera_x != r16(g_ram, 0xB1E8)) right = 1;
    if (stage == 32 && map == 4 && nx < 0) {
        /* This wall is fixed at world X=3400. Sampling that wall rather than
         * the viewport edge avoids reflecting Rocket or the boss projectiles
         * when they cross the left border. Only extend it while it is still
         * at the viewport edge; once the camera passes it, the wall belongs
         * in the room rather than across the entire left margin. */
        int sx = 3400 - (int)camera_x + (-nx - 1) % 8;
        if (sx >= nw - 40 && sx < nw) return sx;
    }
    if (left && nx < 0) return (-nx - 1) % strip;
    if (right && nx >= nw) return nw - 1 - (nx - nw) % strip;
    return INT_MIN;
}
static void scanline(const GVDP *v, int line, const uint32_t *native, int nw,
                     uint32_t *out, int width)
{
    uint32_t palette[3][64];
    for (int i = 0; i < 64; ++i) {
        palette[0][i] = genesis_dac_cram_to_argb(v->cram[i], GENESIS_DAC_NORMAL);
        palette[1][i] = genesis_dac_cram_to_argb(v->cram[i], GENESIS_DAC_SHADOW);
        palette[2][i] = genesis_dac_cram_to_argb(v->cram[i], GENESIS_DAC_HIGHLIGHT);
    }
    unsigned backdrop = v->reg[7] & 63;
    if (!line) {
        ++s_frames; s_width = s_requested_width = width;
        s_terrain_checks = s_terrain_errors = s_lines_wide = s_lines_native = 0;
        if (width > s_capacity) {
            uint8_t *a = malloc((size_t)width), *b = malloc((size_t)width);
            if (a && b) { free(s_spr_idx); free(s_spr_flags); s_spr_idx=a; s_spr_flags=b; s_capacity=width; }
            else { free(a); free(b); }
        }
        select_scene(v);
    }
    const uint8_t *r = s_scene ? s_scene->ram : g_ram;
    /* The presented actor snapshot and the live machine must agree on the
     * scene class; a transition frame keeps its native presentation. */
    int kind = scene_kind(r);
    if (kind != scene_kind(g_ram)) kind = SCENE_NONE;
    if (!line) {
        s_kind = (unsigned)kind;
        FrameRecord *f = &s_frame_ring[s_frame_ring_count++ % FRAME_RING];
        memset(f, 0, sizeof *f);
        f->frame = cmd_server_current_frame(); f->serial = s_scene ? s_scene->serial : 0;
        f->kind = (uint8_t)kind; f->width = (uint16_t)width;
        f->stage = (uint16_t)r16(r,0xFB12); f->map = (uint16_t)r16(r,0xB1D0);
        f->native_camera = (int32_t)r16(r,0xB1D2); f->actor_camera = (int16_t)r16(r,0xB1C8);
        f->sprites = (uint16_t)(s_scene ? s_scene->count : 0);
    }
    FrameRecord *record = frame_record();
    /* H32 and interlace keep the centered native image; shadow/highlight is
     * composited below. */
    if (nw != 320 || !(v->reg[1] & 64) || (v->reg[12] & 6) || s_capacity < width ||
        kind == SCENE_NONE) {
        uint32_t fill = !(v->reg[1] & 64) ? native[0] : palette[0][backdrop];
        for (int x = 0; x < width; ++x) out[x] = fill;
        memcpy(out + (width - nw) / 2, native, (size_t)nw * sizeof *out);
        ++s_lines_native;
        if (record && record->native_lines < 255) ++record->native_lines;
        return;
    }
    ++s_lines_wide;
    int tiled = kind == SCENE_TILED, sh_mode = (v->reg[12] & 8) != 0;
    unsigned hs = (v->reg[13] & 63u) << 10;
    int hm = v->reg[11] & 3, hi = hm == 0 ? 0 : hm == 2 ? line & ~7 : line;
    int ha = (int16_t)r16(v->vram, hs + hi * 4);
    int hb = (int16_t)r16(v->vram, hs + hi * 4 + 2);
    int camera = unwrap(-ha, r16(r, 0xB1D2));
    int cy = unwrap(v->vsram[0], r16(r, 0xB1D6));
    if (!line) {
        /* A tiled room and its view follow the game camera $B1D2. H-scroll
         * adds raster effects on top: screen shake and the lava arena's
         * per-line heat ripple (-1..+2px). Anchoring on line 0's raster X
         * moved the room edge into the neighbouring block column on some
         * ripple phases, so the margin flashed between two block columns.
         * Each line still samples its own raster X below, so the margins
         * ripple and shake together with the native picture. */
        s_camera = tiled ? (int)r16(r,0xB1D2) : camera;
        s_left = view_left(s_camera,width,r);
        if (kind == SCENE_TILED) room_span(r,s_camera,&s_room_lo,&s_room_hi);
        else s_room_lo = s_room_hi = 0;
        select_waves(v);
        s_actor_lo = INT_MIN; s_actor_hi = INT_MAX;
        if (kind == SCENE_TILED) {
            int first, last;
            room_cover(s_room_lo, s_room_hi, &first, &last);
            if (last - first >= 32) { s_actor_lo = first; s_actor_hi = last; }
        }
        survey_planes(v);
    }
    /* Keep the camera anchor fixed across the frame so per-strip/per-line
     * horizontal scroll remains visible instead of cancelling itself out. */
    int origin = s_camera - s_left;
    int center = (width - nw) / 2;
    unsigned map = r16(r,0xB1D0), stage = r16(r,0xFB12);
    unsigned ba = (v->reg[2] & 56u) << 10, bb = (v->reg[4] & 7u) << 13;
    /* Plane B on plane A's name table (the crystal caves' lava reflection
     * re-scans the streamed terrain under H-int vertical scroll) is terrain
     * too: decode it from the layout, or the margins show columns the
     * streamer has not written yet. */
    int shared_b = tiled && bb == ba;
    int camera_b = shared_b ? unwrap(-hb, r16(r, 0xB1D2)) : 0;
    int bg_h = tiled && hm == 2 && !shared_b ? background_scroll(r,line,hb,center-origin) : hb;
    if (line == 128) s_bg_x = -center - bg_h;
    s_origin = origin;
    int wrap_a = !tiled && unauthored(v,0,ha,nw);
    int wrap_b = !shared_b && unauthored(v,1,bg_h + center - origin,nw);
    if (record && line == 128) {
        record->camera = camera; record->view_left = s_left; record->origin = origin;
        record->room_lo = s_room_lo; record->room_hi = s_room_hi;
        record->wrap_a = (uint8_t)wrap_a; record->wrap_b = (uint8_t)wrap_b;
        record->copied = (uint8_t)(origin == center);
        record->sparse_a = (uint8_t)s_sparse[0]; record->sparse_b = (uint8_t)s_sparse[1];
    }

    /* Sprites: front-group snapshot actors, the reserved hardware SAT
     * prefix (player/HUD), then the remaining snapshot actors. */
    memset(s_spr_flags,0,(size_t)width);
    unsigned sat = (v->reg[5] & 0x7Eu) << 9, index = 0;
    int on_line = 0;
    if (s_scene)
        for (unsigned i=0;i<s_scene->count;++i)
            if(s_scene->sprites[i].front)
                draw_sprite(v,&s_scene->sprites[i],line,width,origin,
                            s_scene->camera_x,s_scene->camera_y,sh_mode);
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
            s.size=v->vram[(addr+2)&65535u];s.attr=(uint16_t)r16(v->vram,addr+4);
            draw_sprite(v,&s,line,width,origin,camera,cy,sh_mode);
        }
        index=v->vram[(addr+3)&65535u]&127u;
        if (!index || index>=80) break;
    }
    if (s_scene)
        for (unsigned i=0;i<s_scene->count;++i)
            if(!s_scene->sprites[i].front)
                draw_sprite(v,&s_scene->sprites[i],line,width,origin,
                            s_scene->camera_x,s_scene->camera_y,sh_mode);

    /* The flying city (stage 23) has only a 256px repeating background, even
     * though its camera has no finite level limit. Across a very wide view,
     * five identical skylines strobe behind the player. Beyond the native
     * view the planes dissolve into a smoothed average of the city colors;
     * the moving actors stay sharp above it. Pass 0 samples that average. */
    int fog = stage == 23 && map == 1 && width > 512;
    static unsigned fog_rgb[3];
    for (int pass = fog ? 0 : 1; pass < 2; ++pass) {
        unsigned sum[3] = {0,0,0}, samples = 0;
        int sample_lo = origin < 0 ? 0 : origin, sample_hi = sample_lo + 256;
        for (int x = 0; x < width; ++x) {
            int nx = x - origin, wx = nx + camera;
            int col = 0;
            if (v->reg[11] & 4) {
                /* 2-cell vertical scroll is defined only over the native
                 * columns; the margins continue a recognised generator's
                 * wave, otherwise the nearest edge column. */
                int xc = nx < 0 ? 0 : nx >= nw ? nw - 1 : nx;
                col = (xc >> 4) * 2;
            }
            int va = v->vsram[col] & 1023, vb = v->vsram[col+1] & 1023;
            if ((v->reg[11] & 4) && (nx < 0 || nx >= nw)) {
                int k = nx >= 0 ? nx >> 4 : -((15 - nx) >> 4);
                if (s_wave_sel[0]) va = (int)wave_value(s_wave_sel[0], k);
                if (s_wave_sel[1]) vb = (int)wave_value(s_wave_sel[1], k);
            }
            int wy = unwrap(va,r16(r,0xB1D6)) + line;
            int inside = nx >= 0 && nx < nw;
            int exa = edge_plane_x(stage,map,0,nx,nw);
            int exb = edge_plane_x(stage,map,1,nx,nw);
            int bx = (exb != nx ? exb + origin : wrap_b && !inside ? native_wrap(nx,nw) + origin : x) - center - bg_h;
            int by = line + vb;
            int bridge = tiled && stage == 2 && map == 9 && s_room_hi - s_room_lo == 320;
            int tx = !tiled ? 0 : bridge ? bridge_x(wx,s_room_lo,s_room_hi)
                                         : room_x(wx,s_room_lo,s_room_hi);
            /* The streamer keeps one plane-height window of layout rows
             * from the camera's Y down; plane B (the lava below the caves,
             * vertical scroll $B1BC) shows a later part of that window. */
            int ph = tiles((v->reg[16] >> 4) & 3) * 8, top = (int)r16(r,0xB1D6);
            int wbx = nx + camera_b;
            int wby = top + ((vb + line - top) % ph + ph) % ph;
            int tbx = !shared_b ? 0 : bridge ? bridge_x(wbx,s_room_lo,s_room_hi)
                                             : room_x(wbx,s_room_lo,s_room_hi);
            int pax = (exa != nx ? exa : wrap_a && !inside ? native_wrap(nx,nw) : nx) - ha;
            int sa = tiled || wrap_a || exa != nx ? pax : sparse_x(0,pax,ha,nw);
            int sb = wrap_b || shared_b || exb != nx ? bx : sparse_x(1,bx,bg_h,nw);
            if (sa != INT_MIN) pax = sa;
            if (sb != INT_MIN) bx = sb;
            unsigned a = tiled && exa == nx ? terrain(r,tx,wy) : plane(v,ba,pax,line+va);
            unsigned b = shared_b ? terrain(r,tbx,wby) : plane(v,bb,bx,by);
            unsigned ap = tiled && exa == nx ? pixel(v,a,tx,wy) : pixel(v,a,pax,line+va);
            unsigned bp = shared_b ? pixel(v,b,tbx,wby) : pixel(v,b,bx,by);
            if (sa == INT_MIN) ap = 0;
            if (sb == INT_MIN) bp = 0;
            int a_hi = ap && (a & 0x8000), b_hi = bp && (b & 0x8000);
            if (pass == 0) {
                if (x >= sample_lo && x < sample_hi) {
                    uint32_t c = palette[0][a_hi ? ap : b_hi ? bp : ap ? ap : bp ? bp : backdrop];
                    sum[0] += (c >> 16) & 255u; sum[1] += (c >> 8) & 255u; sum[2] += c & 255u;
                    ++samples;
                }
                continue;
            }
            unsigned f = s_spr_flags[x], s_op = f & SPR_OPAQUE, s_hi = f & SPR_HIGH;
            unsigned idx = s_op && s_hi ? s_spr_idx[x] : a_hi ? ap : b_hi ? bp :
                           s_op ? s_spr_idx[x] : ap ? ap : bp ? bp : backdrop;
            int level = 0;
            if (sh_mode) {
                int shadow = !((s_op && s_hi) || a_hi || b_hi), hilite = 0;
                if (f & SPR_SHADOW) shadow = 1;
                if (f & SPR_HILITE) { if (shadow) shadow = 0; else hilite = 1; }
                level = hilite ? 2 : shadow ? 1 : 0;
            }
            out[x] = palette[level][idx];
            int distance = !fog ? 0 : x < origin ? origin - x :
                           x >= origin + nw ? x - origin - nw + 1 : 0;
            /* Fog covers the planes. A sprite stays sharp wherever it would
             * win over the planes, and also over high planes deep in fog. */
            int sprite_wins = s_op && (s_hi || !(a_hi || b_hi) || distance >= 64);
            if (distance && !sprite_wins) {
                unsigned mix = distance < 128 ? (unsigned)distance * 2u : 256u;
                uint32_t old = out[x];
                unsigned c[3] = { (old >> 16) & 255u, (old >> 8) & 255u, old & 255u };
                for (int k = 0; k < 3; ++k) c[k] = (c[k] * (256u - mix) + fog_rgb[k] * mix) >> 8;
                out[x] = 0xFF000000u | c[0] << 16 | c[1] << 8 | c[2];
            } else if (distance && s_op) out[x] = palette[0][s_spr_idx[x]];
            if (tiled && tx == wx && nx >= 0 && nx < nw && !(x & 7)) {
                unsigned expected = plane(v,ba,nx-ha,line+va);
                ++s_terrain_checks;
                if (pixel(v,a,wx,wy) != pixel(v,expected,wx,wy)) ++s_terrain_errors;
            }
            if (shared_b && tbx == wbx && nx >= 0 && nx < nw && !(x & 7)) {
                unsigned expected = plane(v,bb,nx-hb,line+vb);
                ++s_terrain_checks;
                if (pixel(v,b,wbx,wby) != pixel(v,expected,wbx,wby)) ++s_terrain_errors;
            }
        }
        if (pass == 0 && samples) {
            for (int k = 0; k < 3; ++k) {
                unsigned avg = sum[k] / samples;
                fog_rgb[k] = line <= 32 ? avg : (fog_rgb[k] * 23u + avg) / 24u;
            }
        }
    }
    for (int x = 0; x < width; ++x) {
        int source = native_edge_x(stage,map,r16(r,0xB1D2),x-origin,nw);
        if (source != INT_MIN) out[x] = native[source];
    }
    /* Preserve the hardware-rendered original viewport exactly. Raster
     * effects, special boss sprites and streamed tile changes can differ
     * from our reconstruction; only the added side pixels need rebuilding.
     * At a clamped stage edge the wider camera keeps its own background
     * parallax, so the reconstruction stands there. */
    int window_y=(v->reg[18]&31)*8;
    int window_line=(v->reg[18]&128) ? line>=window_y : line<window_y;
    /* The aircraft room clamps its camera; its transient right-edge dot
     * field is a reconstruction artifact within the still-authored native
     * viewport. */
    int preserve_native = origin == center || fog || (stage == 48 && map == 4);
    if (!window_line && preserve_native) {
        /* Native columns outside the room's covered blocks show a sliver of the
         * neighbouring area (a bridge tower post at X=0); those keep the
         * room continuation drawn above, like the margins. */
        int from = 0, to = nw, first, last;
        if (tiled) {
            room_cover(s_room_lo, s_room_hi, &first, &last);
            if (last - first >= 32) {
                if (first - s_camera > from) from = first - s_camera;
                if (last - s_camera < to) to = last - s_camera;
            }
        }
        if (to > from) memcpy(out+origin+from,native+from,(size_t)(to-from)*sizeof *out);
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
    static const char *kinds[] = { "none", "tiled", "planar" };
    char out[800];
    snprintf(out,sizeof out,"{\"id\":%d,\"enabled\":%d,\"scene\":\"%s\",\"room\":[%u,%u],"
        "\"wide_lines\":%u,\"native_lines\":%u,\"edge_shifts\":%u,\"width\":%d,\"view_left\":%d,"
        "\"camera\":%d,\"native_x\":%d,\"bg_x_128\":%d,\"frames\":%u,\"terrain_checks\":%u,"
        "\"terrain_errors\":%u,\"wave_hits\":%u,\"wave_misses\":%u,\"sprites\":%u,\"spawn_mode\":\"%s\",\"spawned\":%u,\"sprite_overflow\":%u,\"published\":%u,\"displayed\":%u}",
        id,enabled(),kinds[s_kind<3?s_kind:0],s_room_lo,s_room_hi,s_lines_wide,s_lines_native,s_edge_shifts,s_width,s_left,s_camera,s_origin,s_bg_x,s_frames,s_terrain_checks,s_terrain_errors,s_wave_hits,s_wave_misses,
        s_scene?s_scene->count:0,rka_video_spawn_mode(),s_spawned,s_sprite_overflow,s_serial,s_scene?s_scene->serial:0);
    cmd_send_response(out);
}
void rka_video_sprites_command(int id, const char *json)
{
    /* The presented actor snapshot: world (or screen) coordinates as drawn. */
    (void)json;
    static char out[MAX_SPRITES * 110 + 256];
    const Scene *sc = s_scene;
    int n = snprintf(out, sizeof out, "{\"id\":%d,\"serial\":%u,\"camera\":[%d,%d],\"sprites\":[",
                     id, sc ? sc->serial : 0, sc ? sc->camera_x : 0, sc ? sc->camera_y : 0);
    for (unsigned i = 0; sc && i < sc->count && n < (int)sizeof out - 160; ++i) {
        const Sprite *p = &sc->sprites[i];
        n += snprintf(out + n, sizeof out - (size_t)n,
            "%s{\"x\":%d,\"y\":%d,\"attr\":%u,\"size\":%u,\"screen\":%u,\"front\":%u,\"object\":\"%04X\",\"type\":%u}",
            i ? "," : "", p->x, p->y, p->attr, p->size, p->screen, p->front, p->object, p->type);
    }
    snprintf(out + n, sizeof out - (size_t)n, "]}");
    cmd_send_response(out);
}
void rka_video_history_command(int id, const char *json)
{
    /* {"since":N} returns up to 128 frame records with index >= N. */
    unsigned since = 0;
    const char *at = json ? strstr(json, "\"since\"") : NULL;
    if (at && (at = strchr(at, ':'))) since = (unsigned)strtoul(at + 1, NULL, 10);
    unsigned oldest = s_frame_ring_count > FRAME_RING ? s_frame_ring_count - FRAME_RING : 0;
    if (since < oldest) since = oldest;
    static char out[128 * 400 + 128];
    int n = snprintf(out, sizeof out, "{\"id\":%d,\"count\":%u,\"frames\":[", id, s_frame_ring_count);
    for (unsigned i = since, k = 0; i < s_frame_ring_count && k < 128 &&
         n < (int)sizeof out - 512; ++i, ++k) {
        const FrameRecord *f = &s_frame_ring[i % FRAME_RING];
        n += snprintf(out + n, sizeof out - (size_t)n,
            "%s{\"i\":%u,\"frame\":%u,\"serial\":%u,\"stage\":%u,\"map\":%u,\"kind\":%u,\"width\":%u,"
            "\"camera\":%d,\"native_camera\":%d,\"actor_camera\":%d,\"view_left\":%d,\"origin\":%d,"
            "\"room\":[%d,%d],\"wrap\":[%u,%u],\"sparse\":[%u,%u],\"copied\":%u,\"native_lines\":%u,\"sprites\":%u}",
            k ? "," : "", i, f->frame, f->serial, f->stage, f->map, f->kind, f->width, f->camera,
            f->native_camera, f->actor_camera, f->view_left, f->origin, f->room_lo, f->room_hi,
            f->wrap_a, f->wrap_b, f->sparse_a, f->sparse_b, f->copied, f->native_lines, f->sprites);
    }
    snprintf(out + n, sizeof out - (size_t)n, "]}");
    cmd_send_response(out);
}
const GameVideo rka_video = { configure, enabled, width, scanline };
