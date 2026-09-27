#pragma once
#include <stddef.h>
struct RecompLauncherCModProvider;
void rka_load_settings(const char *path);
const struct RecompLauncherCModProvider *rka_mods(const struct RecompLauncherCModProvider *base);
void rka_config_image(char *out, size_t cap);
int rka_config_adopt(const char *line);
