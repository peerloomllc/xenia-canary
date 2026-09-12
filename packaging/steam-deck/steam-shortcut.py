#!/usr/bin/env python3
"""Add (or update) a non-Steam shortcut in Steam's own list, and put its
artwork where Steam looks for it.

Steam stores shortcuts in `shortcuts.vdf`, a binary format, and rewrites the
file when it exits, so this refuses to run while Steam is running rather than
having its work thrown away.

Artwork is named by the shortcut's **unsigned** app id, which the file stores
signed. That id is a CRC of the command and the name, so it is stable across
runs and the artwork keeps matching.

Usage:
  steam-shortcut.py --name "Xenia Canary" --exe /path/to/launcher [--art DIR]
"""
import argparse
import binascii
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

# Binary VDF markers.
MAP, STR, INT, END = 0x00, 0x01, 0x02, 0x08


def read_cstr(data, i):
    j = data.index(b"\x00", i)
    return data[i:j].decode("utf-8", "replace"), j + 1


def parse(data):
    """shortcuts.vdf -> list of entries, each an ordered list of (type, key,
    value). Kept as a list so anything Steam writes that we do not understand
    survives a round trip."""
    i = 0
    if data[:1] == b"\x00":
        _, i = read_cstr(data, 1)  # the outer "shortcuts" map
    entries = []
    while i < len(data) and data[i] != END:
        if data[i] != MAP:
            break
        i += 1
        _, i = read_cstr(data, i)  # the entry's index, which we renumber
        fields = []
        while i < len(data) and data[i] != END:
            t = data[i]
            i += 1
            key, i = read_cstr(data, i)
            if t == STR:
                val, i = read_cstr(data, i)
            elif t == INT:
                val = struct.unpack("<i", data[i:i + 4])[0]
                i += 4
            elif t == MAP:  # tags, and anything else nested
                sub = []
                while i < len(data) and data[i] != END:
                    st = data[i]
                    i += 1
                    sk, i = read_cstr(data, i)
                    if st == STR:
                        sv, i = read_cstr(data, i)
                    else:
                        sv = struct.unpack("<i", data[i:i + 4])[0]
                        i += 4
                    sub.append((st, sk, sv))
                i += 1
                val = sub
            else:
                raise ValueError("unknown field type %#x in shortcuts.vdf" % t)
            fields.append((t, key, val))
        i += 1
        entries.append(fields)
    return entries


def serialise(entries):
    out = bytearray([MAP]) + b"shortcuts\x00"
    for n, fields in enumerate(entries):
        out += bytes([MAP]) + str(n).encode() + b"\x00"
        for t, key, val in fields:
            out += bytes([t]) + key.encode() + b"\x00"
            if t == STR:
                out += str(val).encode() + b"\x00"
            elif t == INT:
                out += struct.pack("<i", int(val))
            else:
                for st, sk, sv in val:
                    out += bytes([st]) + sk.encode() + b"\x00"
                    if st == STR:
                        out += str(sv).encode() + b"\x00"
                    else:
                        out += struct.pack("<i", int(sv))
                out += bytes([END])
        out += bytes([END])
    return bytes(out) + bytes([END, END])


def app_id(exe_quoted, name):
    """The id Steam gives a non-Steam shortcut: a CRC of the command and the
    name, with the top bit set. Stored signed, named unsigned."""
    crc = binascii.crc32((exe_quoted + name).encode()) & 0xFFFFFFFF
    unsigned = crc | 0x80000000
    return struct.unpack("<i", struct.pack("<I", unsigned))[0], unsigned


def field(fields, key):
    for t, k, v in fields:
        if k.lower() == key.lower():
            return v
    return None


def steam_running():
    try:
        out = subprocess.run(["pgrep", "-x", "steam"], capture_output=True)
        return out.returncode == 0
    except FileNotFoundError:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--exe", required=True, help="the launcher to run")
    ap.add_argument("--art", help="folder holding xenia*.png")
    ap.add_argument("--userdata", help="Steam userdata folder, found if absent")
    args = ap.parse_args()

    if steam_running():
        sys.exit("Steam is running. Close it first: it rewrites its shortcut "
                 "list when it exits and would undo this.")

    roots = [Path(args.userdata)] if args.userdata else [
        Path.home() / ".steam/steam/userdata",
        Path.home() / ".local/share/Steam/userdata",
    ]
    users = []
    for root in roots:
        if root.is_dir():
            users += [d for d in root.iterdir() if d.name.isdigit()]
    if not users:
        sys.exit("No Steam user folder found. Has Steam ever run here?")

    exe = str(Path(args.exe).resolve())
    exe_quoted = '"%s"' % exe
    signed, unsigned = app_id(exe_quoted, args.name)

    for user in users:
        config = user / "config"
        config.mkdir(parents=True, exist_ok=True)
        path = config / "shortcuts.vdf"
        data = path.read_bytes() if path.exists() else b""
        entries = parse(data) if data else []

        if path.exists():
            backup = config / "shortcuts.vdf.before-xenia"
            if not backup.exists():
                shutil.copy2(path, backup)

        mine = [e for e in entries if field(e, "Exe") == exe_quoted]
        for e in mine:
            entries.remove(e)
        entries.append([
            (INT, "appid", signed),
            (STR, "AppName", args.name),
            (STR, "Exe", exe_quoted),
            (STR, "StartDir", '"%s"' % str(Path(exe).parent)),
            (STR, "icon", ""),
            (STR, "ShortcutPath", ""),
            (STR, "LaunchOptions", ""),
            (INT, "IsHidden", 0),
            (INT, "AllowDesktopConfig", 1),
            (INT, "AllowOverlay", 1),
            (INT, "OpenVR", 0),
            (INT, "Devkit", 0),
            (STR, "DevkitGameID", ""),
            (INT, "DevkitOverrideAppID", 0),
            (INT, "LastPlayTime", 0),
            (STR, "FlatpakAppID", ""),
            (MAP, "tags", [(STR, "0", "Emulator")]),
        ])
        path.write_bytes(serialise(entries))
        print("%s %r in %s" % ("Updated" if mine else "Added", args.name, path))

        if args.art:
            art = Path(args.art)
            grid = config / "grid"
            grid.mkdir(parents=True, exist_ok=True)
            for src, dst in (("xeniap.png", "%dp.png" % unsigned),
                             ("xenia.png", "%d.png" % unsigned),
                             ("xenia_hero.png", "%d_hero.png" % unsigned),
                             ("xenia_logo.png", "%d_logo.png" % unsigned)):
                if (art / src).exists():
                    shutil.copy2(art / src, grid / dst)
            print("Artwork copied to %s (app id %d)" % (grid, unsigned))


if __name__ == "__main__":
    main()
