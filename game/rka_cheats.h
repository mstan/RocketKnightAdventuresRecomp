#ifndef RKA_CHEATS_H
#define RKA_CHEATS_H
#include <stdint.h>
#include <stdio.h>
/* Playtest cheats, each an independent mod (see rka_cheats.c). */
int rka_cheats_hook(uint32_t pc);
int rka_cheats_count(void);
const char *rka_cheats_id(int i);
const char *rka_cheats_name(int i);
const char *rka_cheats_description(int i);
int rka_cheats_find(const char *id);
int rka_cheats_on(int i);
void rka_cheats_enable(int i, int on);
int rka_cheats_stage(void);
int rka_cheats_set_stage(const char *value);
void rka_cheats_off(void);
void rka_cheats_image(char *out, size_t cap);
int rka_cheats_adopt(const char *text);
void rka_cheats_save(FILE *f);
void rka_cheats_load_line(const char *line);
#endif
