// Force-included into every game/PsyX translation unit on the ESP32 build.
// Routes stdio file access through the "/d" game-data VFS (esp_fs.c) and
// supplies the few MSVC/POSIX helpers newlib lacks.
#pragma once

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
void* esp_game_fopen(const char* name, const char* mode);
#ifdef __cplusplus
}
#endif

#define fopen(name, mode) ((FILE*)esp_game_fopen((name), (mode)))

#ifndef _stricmp
#define _stricmp  strcasecmp
#endif
#ifndef _strnicmp
#define _strnicmp strncasecmp
#endif

// Profile/save paths: the ESP32 VFS has no directories and getenv("HOME")
// does not exist, so config.dat/progress.dat land in the RAM-file area.
#define HOME_ENV "HOME"
#define _mkdir(p) (0)
