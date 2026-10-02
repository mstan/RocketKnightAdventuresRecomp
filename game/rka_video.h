#pragma once
#include "game_video.h"
#include <stdint.h>
extern const GameVideo rka_video;
int rka_video_hook(uint32_t pc);
void rka_video_reset(void);
void rka_video_command(int id, const char *json);
void rka_spawns_command(int id, const char *json);
void rka_video_history_command(int id, const char *json);
void rka_video_sprites_command(int id, const char *json);
int rka_video_set_spawn(const char *mode);
const char *rka_video_spawn_mode(void);
