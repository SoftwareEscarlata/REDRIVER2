#!/usr/bin/env python3
"""Pack the Driver 2 demo data into the ESP32 'gamedata' flash image.

Container ("OLVL" format, shared with the OpenLara/Descent ports):
    u32 magic 'OLVL' | u32 count | count * {char name[32]; u32 off; u32 size}
    then 4-byte-aligned file blobs, so mmap'd pointers are usable in place.

Names keep their relative path with '/' separators (the ESP32 VFS matches
case-insensitively, and the game's backslash paths are normalized).

Dropped from the PC set: GFX/HQ (4.7MB of PC-only high-res font TGAs) and
SPLASH1N.TIM. Sound banks are optional (--no-sound) until audio exists.

Usage: python make_data.py <out.bin> [DRIVER2DEMO dir] [--no-sound]
"""
import os
import struct
import sys

MAGIC = 0x4C564C4F  # 'OLVL'
NAME_LEN = 32

SKIP_DIRS = {"GFX/HQ"}
SKIP_FILES = {"GFX/SPLASH1N.TIM"}
SOUND_FILES = {"SOUND/VOICES2.BLK", "SOUND/MUSIC.BIN"}


def collect(root, with_sound):
    out = []
    for dirpath, _dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root).replace("\\", "/")
        if rel_dir == ".":
            rel_dir = ""
        if any(rel_dir == d or rel_dir.startswith(d + "/") for d in SKIP_DIRS):
            continue
        for fn in filenames:
            rel = (rel_dir + "/" + fn) if rel_dir else fn
            if rel.upper() in {s.upper() for s in SKIP_FILES}:
                continue
            if not with_sound and rel.upper() in {s.upper() for s in SOUND_FILES}:
                continue
            if len(rel) >= NAME_LEN:
                sys.exit(f"path too long for container: {rel}")
            out.append((rel, os.path.join(dirpath, fn)))
    return sorted(out)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    out_path = sys.argv[1]
    root = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") \
        else os.path.join(os.path.dirname(__file__), "..", "data", "DRIVER2DEMO")
    with_sound = "--no-sound" not in sys.argv

    files = collect(root, with_sound)
    entry_size = NAME_LEN + 8
    off = (8 + len(files) * entry_size + 3) & ~3

    entries, blobs = [], []
    for rel, full in files:
        blob = open(full, "rb").read()
        entries.append((rel, off, len(blob)))
        blobs.append(blob)
        off = (off + len(blob) + 3) & ~3

    with open(out_path, "wb") as out:
        out.write(struct.pack("<II", MAGIC, len(files)))
        for rel, o, sz in entries:
            out.write(struct.pack(f"<{NAME_LEN}sII", rel.encode(), o, sz))
        pos = 8 + len(files) * entry_size
        for (rel, o, sz), blob in zip(entries, blobs):
            out.write(b"\0" * (o - pos))
            out.write(blob)
            pos = o + sz

    total = os.path.getsize(out_path)
    print(f"{out_path}: {len(files)} files, {total/1024/1024:.2f} MB"
          f" ({'with' if with_sound else 'no'} sound)")
    for rel, o, sz in entries:
        if sz > 100000:
            print(f"  {rel:28s} {sz/1024/1024:6.2f} MB")


if __name__ == "__main__":
    main()
