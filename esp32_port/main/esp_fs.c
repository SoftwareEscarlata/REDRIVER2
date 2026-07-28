// OLVL flash container + RAM write-files, exposed as an ESP-IDF VFS at "/d".
//
// Reads come straight from memory-mapped flash (zero RAM). Writes (descent.cfg,
// pilot files, savegames) go to grow-on-demand PSRAM buffers that live for the
// session; NVS persistence can come later.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#include "esp_vfs.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "esp_fs.h"

static const char* TAG = "esp_fs";

#define OLVL_MAGIC 0x4C564C4F

typedef struct {
    char name[32];
    uint32_t offset;
    uint32_t size;
} OlvlEntry;

static const uint8_t* sBase;
static const OlvlEntry* sFiles;
static uint32_t sCount;

// ---- RAM write files ------------------------------------------------------
#define MAX_RAM_FILES 12

typedef struct {
    char name[32];
    uint8_t* data;      // PSRAM
    uint32_t size;
    uint32_t cap;
    int used;
} RamFile;

static RamFile sRam[MAX_RAM_FILES];

// ---- open file table ------------------------------------------------------
#define MAX_OPEN 12

typedef struct {
    int used;
    const uint8_t* ro;  // flash pointer (read-only) or NULL
    RamFile* rw;        // RAM file or NULL
    uint32_t size;
    uint32_t pos;
    int writable;
} OpenFile;

static OpenFile sOpen[MAX_OPEN];

// case-insensitive, treating '\' and '/' as the same separator (the game
// uses DOS-style paths like "DATA\GFX.RAW")
static int nameEq(const char* a, const char* b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

static const OlvlEntry* flashFind(const char* name)
{
    for (uint32_t i = 0; i < sCount; i++)
        if (nameEq(sFiles[i].name, name)) return &sFiles[i];
    return NULL;
}

static RamFile* ramFind(const char* name)
{
    for (int i = 0; i < MAX_RAM_FILES; i++)
        if (sRam[i].used && nameEq(sRam[i].name, name)) return &sRam[i];
    return NULL;
}

// ---- VFS callbacks --------------------------------------------------------
static int vfs_open(const char* path, int flags, int mode)
{
    (void)mode;
    if (*path == '/') path++;

    int fd = -1;
    for (int i = 0; i < MAX_OPEN; i++)
        if (!sOpen[i].used) { fd = i; break; }
    if (fd < 0) { errno = ENFILE; return -1; }

    OpenFile* f = &sOpen[fd];
    memset(f, 0, sizeof(*f));

    int wr = (flags & (O_WRONLY | O_RDWR | O_CREAT)) != 0;

    // RAM files shadow flash (a saved config wins over a shipped one)
    RamFile* rf = ramFind(path);
    if (rf && !wr) {
        f->used = 1; f->rw = rf; f->size = rf->size;
        return fd;
    }

    if (wr) {
        if (!rf) {
            for (int i = 0; i < MAX_RAM_FILES; i++)
                if (!sRam[i].used) { rf = &sRam[i]; break; }
            if (!rf) { errno = ENFILE; return -1; }
            memset(rf, 0, sizeof(*rf));
            strncpy(rf->name, path, sizeof(rf->name) - 1);
            rf->used = 1;
        }
        if (!(flags & O_APPEND)) rf->size = 0;
        f->used = 1; f->rw = rf; f->writable = 1;
        f->pos = (flags & O_APPEND) ? rf->size : 0;
        return fd;
    }

    const OlvlEntry* e = flashFind(path);
    if (!e) { errno = ENOENT; return -1; }
    f->used = 1; f->ro = sBase + e->offset; f->size = e->size;
    return fd;
}

static ssize_t vfs_read(int fd, void* dst, size_t len)
{
    OpenFile* f = &sOpen[fd];
    if (!f->used) { errno = EBADF; return -1; }
    const uint8_t* src = f->ro ? f->ro : (f->rw ? f->rw->data : NULL);
    uint32_t size = f->ro ? f->size : (f->rw ? f->rw->size : 0);
    if (!src) return 0;
    if (f->pos >= size) return 0;
    if (f->pos + len > size) len = size - f->pos;
    memcpy(dst, src + f->pos, len);
    f->pos += len;
    return len;
}

static ssize_t vfs_write(int fd, const void* src, size_t len)
{
    OpenFile* f = &sOpen[fd];
    if (!f->used || !f->writable || !f->rw) { errno = EBADF; return -1; }
    RamFile* rf = f->rw;
    uint32_t need = f->pos + len;
    if (need > rf->cap) {
        uint32_t cap = rf->cap ? rf->cap : 1024;
        while (cap < need) cap *= 2;
        uint8_t* nd = heap_caps_realloc(rf->data, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nd) { errno = ENOSPC; return -1; }
        rf->data = nd; rf->cap = cap;
    }
    memcpy(rf->data + f->pos, src, len);
    f->pos += len;
    if (f->pos > rf->size) rf->size = f->pos;
    return len;
}

static off_t vfs_lseek(int fd, off_t off, int whence)
{
    OpenFile* f = &sOpen[fd];
    if (!f->used) { errno = EBADF; return -1; }
    uint32_t size = f->ro ? f->size : (f->rw ? f->rw->size : 0);
    int64_t np = (whence == SEEK_SET) ? off
               : (whence == SEEK_CUR) ? (int64_t)f->pos + off
               : (int64_t)size + off;
    if (np < 0) { errno = EINVAL; return -1; }
    f->pos = (uint32_t)np;
    return f->pos;
}

static int vfs_close(int fd)
{
    if (!sOpen[fd].used) { errno = EBADF; return -1; }
    sOpen[fd].used = 0;
    return 0;
}

static int vfs_fstat(int fd, struct stat* st)
{
    OpenFile* f = &sOpen[fd];
    if (!f->used) { errno = EBADF; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_size = f->ro ? f->size : (f->rw ? f->rw->size : 0);
    st->st_mode = S_IFREG;
    return 0;
}

static int vfs_stat(const char* path, struct stat* st)
{
    if (*path == '/') path++;
    memset(st, 0, sizeof(*st));
    RamFile* rf = ramFind(path);
    if (rf) { st->st_size = rf->size; st->st_mode = S_IFREG; return 0; }
    const OlvlEntry* e = flashFind(path);
    if (!e) { errno = ENOENT; return -1; }
    st->st_size = e->size;
    st->st_mode = S_IFREG;
    return 0;
}

static int vfs_unlink(const char* path)
{
    if (*path == '/') path++;
    RamFile* rf = ramFind(path);
    if (rf) { rf->used = 0; return 0; }
    errno = ENOENT;
    return -1;
}

// ---- public ---------------------------------------------------------------
int esp_fs_mount(void)
{
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "gamedata");
    if (!part) { ESP_LOGE(TAG, "gamedata partition missing"); return -1; }

    const void* map;
    esp_partition_mmap_handle_t h;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &map, &h) != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed");
        return -1;
    }

    const uint32_t* hdr = (const uint32_t*)map;
    if (hdr[0] != OLVL_MAGIC) {
        ESP_LOGE(TAG, "bad OLVL magic %08lx — flash game data first", (unsigned long)hdr[0]);
        return -1;
    }
    sBase = (const uint8_t*)map;
    sCount = hdr[1];
    sFiles = (const OlvlEntry*)(sBase + 8);
    ESP_LOGI(TAG, "gamedata: %lu files mounted at %p", (unsigned long)sCount, map);

    esp_vfs_t vfs = { 0 };
    vfs.flags = ESP_VFS_FLAG_DEFAULT;
    vfs.open = vfs_open;
    vfs.read = vfs_read;
    vfs.write = vfs_write;
    vfs.lseek = vfs_lseek;
    vfs.close = vfs_close;
    vfs.fstat = vfs_fstat;
    vfs.stat = vfs_stat;
    vfs.unlink = vfs_unlink;
    return esp_vfs_register("/d", &vfs, NULL) == ESP_OK ? 0 : -1;
}

void* esp_game_fopen(const char* name, const char* mode)
{
    if (name[0] == '/')
        return fopen(name, mode);
    char buf[80];
    snprintf(buf, sizeof(buf), "/d/%s", name);
    return fopen(buf, mode);
}
