/* RKA USA playtest cheats, equivalent to the published Game Genie codes.
 * Generated code cannot be byte-patched, so each code is an instruction hook
 * that produces the patched instruction's effect (see CUSTOM-VIDEO.md):
 *
 *   Start on level 2-7  ATFA-AAET..F6FA-AAET  $000A90: move.w #$2C,$FB12 -> area
 *                       (level 3 corrected to area 40, see s_start_area)
 *   Invincible          AD4A-AA48/AD4A-AA5L   $F45E/$F46A: bne.w -> bra.w
 *   Infinite health     AX4T-AA4L             $F54A: skip sub.w d1,$40(a1)
 *   Infinite lives      ATNA-CA64             $1189A: skip the sbcd at $1189E
 *
 * The start area replaces only the new-game opening ($0A8E); the game then
 * progresses, continues and game-overs through its own code.
 */
#include "rka_cheats.h"
#include "genesis_runtime.h"
#include <stdio.h>
#include <string.h>

/* New-game entry areas for $FFFB12, from the game's next-area table at
 * $8322: opening 44 -> stage 1 (1,2,3,11) -> 2 (4-9) -> 3 (40-43) ->
 * 4 (45,46,13,12,14,15) -> 5 (48,23,24) -> 6 (49 intro,25,26) -> 7 (47,27-35)
 * -> ending (50-53). The published level codes agree except "Start on
 * Level 3" (HJFA-AAET, $3A): area 58's successor is 1, so it restarts
 * stage 1. Stage 3 starts at 40, the successor of stage 2's last area. */
static const uint8_t s_start_area[8] = { 0, 44, 4, 40, 45, 48, 49, 47 };
/* Each cheat is its own mod: an independent enable, plus the start stage. */
enum { CHEAT_STAGE, CHEAT_LIVES, CHEAT_HEALTH, CHEAT_INVINCIBLE, CHEAT_COUNT };
static const struct { const char *id, *name, *description, *ini; } s_cheats[CHEAT_COUNT] = {
    { "start-stage", "Start Stage",
      "A new game begins at the chosen stage (the Game Genie start-level codes, "
      "with stage 3 corrected). Later stages, Continue and Game Over follow normally.", "start_stage_enabled" },
    { "infinite-lives", "Infinite Lives",
      "Losing a life never reduces the count (Game Genie ATNA-CA64).", "infinite_lives" },
    { "infinite-health", "Infinite Health",
      "Hits never reduce health (Game Genie AX4T-AA4L).", "infinite_health" },
    { "invincible", "Invincible",
      "Sparkster ignores damage on foot and while flying (Game Genie AD4A-AA5L/AD4A-AA48).", "invincible" },
};
static int s_on[CHEAT_COUNT], s_stage = 1;

int rka_cheats_count(void) { return CHEAT_COUNT; }
const char *rka_cheats_id(int i) { return i >= 0 && i < CHEAT_COUNT ? s_cheats[i].id : NULL; }
const char *rka_cheats_name(int i) { return i >= 0 && i < CHEAT_COUNT ? s_cheats[i].name : ""; }
const char *rka_cheats_description(int i) { return i >= 0 && i < CHEAT_COUNT ? s_cheats[i].description : ""; }
int rka_cheats_find(const char *id)
{
    for (int i = 0; id && i < CHEAT_COUNT; ++i)
        if (!strcmp(id, s_cheats[i].id)) return i;
    return -1;
}
int rka_cheats_on(int i) { return i >= 0 && i < CHEAT_COUNT && s_on[i]; }
void rka_cheats_enable(int i, int on) { if (i >= 0 && i < CHEAT_COUNT) s_on[i] = on != 0; }
int rka_cheats_stage(void) { return s_stage; }
int rka_cheats_set_stage(const char *value)
{
    if (!value || value[0] < '1' || value[0] > '7' || value[1]) return 0;
    s_stage = value[0] - '0';
    return 1;
}
/* Session image fragment "cheats=S,L,H,I": the start stage (0 = off) and the
 * three toggles (the whole game line is 48 bytes). */
void rka_cheats_image(char *out, size_t cap)
{
    snprintf(out, cap, "cheats=%d,%d,%d,%d", s_on[CHEAT_STAGE] ? s_stage : 0,
             s_on[CHEAT_LIVES], s_on[CHEAT_HEALTH], s_on[CHEAT_INVINCIBLE]);
}
int rka_cheats_adopt(const char *text)
{
    int st, l, h, i;
    if (sscanf(text, "cheats=%d,%d,%d,%d", &st, &l, &h, &i) != 4 || st < 0 || st > 7) return 0;
    s_on[CHEAT_STAGE] = st != 0; if (st) s_stage = st;
    s_on[CHEAT_LIVES] = l != 0; s_on[CHEAT_HEALTH] = h != 0; s_on[CHEAT_INVINCIBLE] = i != 0;
    return 1;
}
void rka_cheats_off(void) { memset(s_on, 0, sizeof s_on); }
void rka_cheats_save(FILE *f)
{
    for (int i = 0; i < CHEAT_COUNT; ++i) fprintf(f, "%s=%d\n", s_cheats[i].ini, s_on[i]);
    fprintf(f, "start_stage=%d\n", s_stage);
}
void rka_cheats_load_line(const char *line)
{
    char key[32];
    int v;
    if (sscanf(line, "%31[^=]=%d", key, &v) != 2) return;
    if (!strcmp(key, "start_stage")) { if (v >= 1 && v <= 7) s_stage = v; return; }
    for (int i = 0; i < CHEAT_COUNT; ++i)
        if (!strcmp(key, s_cheats[i].ini)) s_on[i] = v != 0;
}

static void ram16(unsigned address, unsigned value)
{
    g_ram[address & 0xFFFFu] = (uint8_t)(value >> 8);
    g_ram[(address + 1) & 0xFFFFu] = (uint8_t)value;
}
int rka_cheats_hook(uint32_t pc)
{
    switch (pc) {
    case 0x000A94:   /* after the new game stores its opening area */
        if (s_on[CHEAT_STAGE] && s_stage > 1) ram16(0xFB12, s_start_area[s_stage]);
        break;
    case 0x00F45E: case 0x00F46A:   /* bne.w over the damage path: take it */
        if (s_on[CHEAT_INVINCIBLE]) g_cpu.SR = (uint16_t)(g_cpu.SR & ~SR_Z);
        break;
    case 0x00F54A: {   /* sub.w d1,$40(a1): pre-add so the health is unchanged */
        if (!s_on[CHEAT_HEALTH]) break;
        unsigned o = g_cpu.A[1] & 0xFFFFu;
        unsigned health = (g_ram[(o + 0x40) & 0xFFFFu] << 8) | g_ram[(o + 0x41) & 0xFFFFu];
        ram16(o + 0x40, health + (g_cpu.D[1] & 0xFFFFu));
        break;
    }
    case 0x01189E: {   /* sbcd -(a2),-(a0) on $FB0D: pre-increment (BCD) */
        if (!s_on[CHEAT_LIVES]) break;
        unsigned lives = g_ram[0xFB0D];
        if (lives < 0x99) g_ram[0xFB0D] = (uint8_t)((lives & 15) == 9 ? (lives & 0xF0) + 0x10 : lives + 1);
        break;
    }
    }
    return 0;
}
