#!/usr/bin/env python3
"""Fetch the free Driver 2 demo data set (DRIVER2DEMO) that the OpenDriver2
project serves for its official browser demo, and extract it next to the game.

Usage:  python get_demo_data.py [dest_dir=../data]

Downloads webgame/REDRIVER2.data (~30MB) + its emscripten manifest from
opendriver2.github.io and unpacks the DRIVER2DEMO folder (demo Havana level,
frontend assets, sounds, one mission) plus demo_config.ini.
"""
import json
import os
import re
import sys
import urllib.request

BASE = "https://raw.githubusercontent.com/OpenDriver2/opendriver2.github.io/master/webgame/"


def fetch(name):
    print(f"downloading {name} ...")
    with urllib.request.urlopen(BASE + name) as r:
        return r.read()


def main():
    dest = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "data")

    js = fetch("REDRIVER2.js").decode("utf-8", errors="replace")
    data = fetch("REDRIVER2.data")

    m = re.search(r"loadPackage\((\{.*?\})\);", js, re.S)
    if not m:
        sys.exit("manifest not found in REDRIVER2.js — package format changed?")
    meta = json.loads(m.group(1))

    count = 0
    for f in meta["files"]:
        name = f["filename"].lstrip("/")
        # only the demo data set + its config; skip web-build logs etc.
        if not (name.startswith("DRIVER2DEMO") or name == "demo_config.ini"):
            continue
        path = os.path.join(dest, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as out:
            out.write(data[f["start"]:f["end"]])
        count += 1

    print(f"{count} files -> {os.path.abspath(dest)}")
    print("To use: copy demo_config.ini over config.ini (sets dataFolder=DRIVER2DEMO)")


if __name__ == "__main__":
    main()
