// Read-only game-data filesystem over the mmap'd OLVL flash container,
// plus session-lifetime RAM files for writes (config, pilot, saves).
// Registered as an ESP-IDF VFS at "/d" so the game's plain stdio works.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// mmap the gamedata partition, parse the OLVL container, register the VFS.
// Returns 0 on success.
int esp_fs_mount(void);

// fopen wrapper the whole game is redirected to (see esp_compat.h):
// relative paths resolve inside "/d" (case-insensitive).
void* esp_game_fopen(const char* name, const char* mode);

#ifdef __cplusplus
}
#endif
