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

// Direct pointer to a container file inside memory-mapped flash, or NULL if it
// is not in the container. Lets bulk read-only data (the level spool lump) be
// used in place instead of copied into RAM. `size` may be NULL.
const void* esp_fs_map(const char* name, unsigned int* size);

#ifdef __cplusplus
}
#endif
