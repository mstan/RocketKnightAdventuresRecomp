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
 * ($2D7E). Map 9 stops streaming for a boss arena; inside a tiled stage its
 * layout stays valid ($B1E4 is a real width mask), while the ending's map 9
 * has none. Map 1 (vehicles, shooters, set pieces, cutscenes) never streams:
 * its planes are loaded whole and wrap at the plane width, so the live name
 * tables are valid beyond 320px. Every playable scene loads a block
 * dictionary at $B1F6; the Continue screen and stage cards do not, and keep
 * their centered presentation. */
enum { SCENE_NONE, SCENE_TILED, SCENE_PLANAR };
static int scene_kind(const uint8_t *r)
{
    unsigned mode = r16(r, 0xB002), map = r16(r, 0xB1D0);
    if ((mode != 4 && mode != 7) || !r32(r, 0xB1F6)) return SCENE_NONE;
    if (map >= 2 && map <= 8) return r32(r, 0xB1FA) ? SCENE_TILED : SCENE_NONE;
    if (map == 9 && r32(r, 0xB1FA) && r16(r, 0xB1E4) != 0xFFFF) return SCENE_TILED;
    return map == 1 || map == 9 ? SCENE_PLANAR : SCENE_NONE;
}
static int stage_width(const uint8_t *r) { return (int)r16(r, 0xB1E8) + 320; }
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
static int room_x(int wx, int lo, int hi)
{
    int first = (lo + 31) & ~31, last = hi & ~31;
    if (last - first < 32) return wx;
    if (wx < lo) return first + ((wx - first) & 31);
    if (wx >= hi) return last - 32 + ((wx - last) & 31);
    return wx;
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
    s_scene = NULL; s_serial = s_frames = s_spawned = s_edge_shifts = 0;
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
    int view = view_left(camera,s_requested_width,g_ram), lo, hi;
    /* Widen only across the room the view really shows. Margins beyond a
     * locked room are continued wall, not a reason to wake the neighbouring
     * area; the native camera-32..camera+352 range always remains. */
    room_span(g_ram,camera,&lo,&hi);
    int left = (view > lo ? view : lo) - 32;
    int right = (view+s_requested_width < hi ? view+s_requested_width : hi) + 32;
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
    for (unsigned i = since, k = 0; i < s_spawn_count && k < 256; ++i, ++k) {
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
typedef struct { uint32_t pc; uint8_t camera, zero_left; } EdgeSpawn;
static const EdgeSpawn s_edge_spawns[] = {
    { 0x0138E8u, 1, 0 }, /* $0138DC move.w $b1d2.w,$6(a2) ; addi #$180 */
    { 0x016E04u, 0, 1 }, /* $016DFE type $96 X from the 0/320 table $16E46 */
    { 0x016E38u, 0, 1 }, /* $016E34 type $9A clr.w $6(a2), enters at +1px */
    { 0x01986Au, 1, 0 }, /* $01985E type $F6 at camera+$160 */
    { 0x020ED6u, 1, 0 }, /* $020EC4 type $169 at camera-$20+$180 */
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
    if (pc == 0x113D8 && scene_kind(g_ram) == SCENE_TILED && r16(g_ram,0xB1D0) != 9) { spawn_visible();return 1; }
    if (pc == 0x115EA) ++s_spawned;
    if (pc == 0x21B4) {
        unsigned o = g_cpu.A[1] & 65535u;
        int x = (int16_t)r16(g_ram,o+0x28);
        /* Expanded culling keeps distant apples alive for the wide renderer,
         * but their native SAT X is only 9 bits. Skip the hardware mapping
         * when its X would wrap into the original 320px viewport. Their
         * unwrapped mapping was already captured at $21AE for the sides. */
        if (r16(g_ram,o) == 0x64 && !(g_ram[o+0x27] & 4) &&
            (x < 0 || x >= 512))
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
        int kind = scene_kind(g_ram);
        if (edge->camera ? kind != SCENE_TILED : map != 1) return 0;
        unsigned slot = g_cpu.A[2] & 65535u;
        int x = (int16_t)r16(g_ram,slot+6), sx = edge->camera ? x - camera : x;
        int shift = sx < 0 || (edge->zero_left && sx == 0) ? -extra_left : sx >= 320 ? extra_right : 0;
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
static void draw_sprite(const GVDP *v, const Sprite *s, int line, int width,
                        int origin, int camera_x, int camera_y, int sh_mode)
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
/* Per-frame name-table column survey: a column is blank when every row
 * selects a fully transparent pattern. Cutscenes author only the 320px they
 * show; such a plane's unseen columns are all blank. */
static uint8_t s_blank_tile[2048], s_blank_col[2][128];
static void survey_planes(const GVDP *v)
{
    for (unsigned t = 0; t < 2048; ++t) {
        const uint8_t *pattern = v->vram + t * 32u;
        unsigned any = 0;
        for (unsigned i = 0; i < 32; ++i) any |= pattern[i];
        s_blank_tile[t] = !any;
    }
    int wt = tiles(v->reg[16] & 3), ht = tiles((v->reg[16] >> 4) & 3);
    unsigned base[2] = { (v->reg[2] & 56u) << 10, (v->reg[4] & 7u) << 13 };
    for (int p = 0; p < 2; ++p)
        for (int c = 0; c < wt && c < 128; ++c) {
            int blank = 1;
            for (int y = 0; y < ht && blank; ++y)
                blank = s_blank_tile[r16(v->vram, base[p] + (unsigned)(y * wt + c) * 2u) & 2047u];
            s_blank_col[p][c] = (uint8_t)blank;
        }
}
/* True when this line's margins of a plane sample only blank columns while
 * the native view samples authored ones: the plane then repeats its native
 * 320px instead of showing empty name-table space. */
static int unauthored(const GVDP *v, int p, int scroll, int width, int origin, int nw)
{
    int wt = tiles(v->reg[16] & 3), authored = 0;
    for (int nx = -origin; nx < width - origin; nx += 8) {
        int inside = nx >= 0 && nx < nw;
        int c = (((nx - scroll) >> 3) & (wt - 1)) & 127;
        if (!inside && !s_blank_col[p][c]) return 0;
        if (inside && !s_blank_col[p][c]) authored = 1;
    }
    int c = (((width - origin - 1 - scroll) >> 3) & (wt - 1)) & 127;
    return authored && s_blank_col[p][c];
}
static int native_wrap(int nx, int nw) { return ((nx % nw) + nw) % nw; }
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
    if (!line) s_kind = (unsigned)kind;
    /* H32 and interlace keep the centered native image; shadow/highlight is
     * composited below. */
    if (nw != 320 || !(v->reg[1] & 64) || (v->reg[12] & 6) || s_capacity < width ||
        kind == SCENE_NONE) {
        uint32_t fill = !(v->reg[1] & 64) ? native[0] : palette[0][backdrop];
        for (int x = 0; x < width; ++x) out[x] = fill;
        memcpy(out + (width - nw) / 2, native, (size_t)nw * sizeof *out);
        ++s_lines_native;
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
        s_camera = camera; s_left = view_left(camera,width,r);
        if (kind == SCENE_TILED) room_span(r,camera,&s_room_lo,&s_room_hi);
        else s_room_lo = s_room_hi = 0;
        survey_planes(v);
    }
    /* Keep the camera anchor fixed across the frame so per-strip/per-line
     * horizontal scroll remains visible instead of cancelling itself out. */
    int origin = s_camera - s_left;
    int center = (width - nw) / 2;
    unsigned map = r16(r,0xB1D0), stage = r16(r,0xFB12);
    int bg_h = tiled && hm == 2 ? background_scroll(r,line,hb,center-origin) : hb;
    if (line == 128) s_bg_x = -center - bg_h;
    s_origin = origin;
    unsigned ba = (v->reg[2] & 56u) << 10, bb = (v->reg[4] & 7u) << 13;
    int wrap_a = !tiled && unauthored(v,0,ha,width,origin,nw);
    int wrap_b = unauthored(v,1,bg_h + center - origin,width,origin,nw);

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
                 * columns; the margins use the nearest edge column. */
                int xc = nx < 0 ? 0 : nx >= nw ? nw - 1 : nx;
                col = (xc >> 4) * 2;
            }
            int va = v->vsram[col] & 1023, vb = v->vsram[col+1] & 1023;
            int wy = unwrap(va,r16(r,0xB1D6)) + line;
            int inside = nx >= 0 && nx < nw;
            int bx = (wrap_b && !inside ? native_wrap(nx,nw) + origin : x) - center - bg_h;
            int by = line + vb;
            int tx = tiled ? room_x(wx,s_room_lo,s_room_hi) : 0;
            int pax = (wrap_a && !inside ? native_wrap(nx,nw) : nx) - ha;
            unsigned a = tiled ? terrain(r,tx,wy) : plane(v,ba,pax,line+va);
            unsigned b = plane(v,bb,bx,by);
            unsigned ap = tiled ? pixel(v,a,tx,wy) : pixel(v,a,pax,line+va);
            unsigned bp = pixel(v,b,bx,by);
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
            if (tiled && nx >= 0 && nx < nw && !(x & 7)) {
                unsigned expected = plane(v,ba,nx-ha,line+va);
                ++s_terrain_checks;
                if (pixel(v,a,wx,wy) != pixel(v,expected,wx,wy)) ++s_terrain_errors;
            }
        }
        if (pass == 0 && samples) {
            for (int k = 0; k < 3; ++k) {
                unsigned avg = sum[k] / samples;
                fog_rgb[k] = line <= 32 ? avg : (fog_rgb[k] * 23u + avg) / 24u;
            }
        }
    }
    /* Preserve the hardware-rendered original viewport exactly. Raster
     * effects, special boss sprites and streamed tile changes can differ
     * from our reconstruction; only the added side pixels need rebuilding.
     * At a clamped stage edge the wider camera keeps its own background
     * parallax, so the reconstruction stands there. */
    int window_y=(v->reg[18]&31)*8;
    int window_line=(v->reg[18]&128) ? line>=window_y : line<window_y;
    if (!window_line && (origin == center || fog))
        memcpy(out+origin,native,(size_t)nw*sizeof *out);
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
        "\"terrain_errors\":%u,\"sprites\":%u,\"spawn_mode\":\"%s\",\"spawned\":%u,\"sprite_overflow\":%u,\"published\":%u,\"displayed\":%u}",
        id,enabled(),kinds[s_kind<3?s_kind:0],s_room_lo,s_room_hi,s_lines_wide,s_lines_native,s_edge_shifts,s_width,s_left,s_camera,s_origin,s_bg_x,s_frames,s_terrain_checks,s_terrain_errors,
        s_scene?s_scene->count:0,rka_video_spawn_mode(),s_spawned,s_sprite_overflow,s_serial,s_scene?s_scene->serial:0);
    cmd_send_response(out);
}
const GameVideo rka_video = { configure, enabled, width, scanline };
