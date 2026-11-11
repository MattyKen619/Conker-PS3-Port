#!/usr/bin/env python3
"""Release build: marks a PNG as a stand-in XMB icon. The package carries it; on its first start the game
replaces a marked icon with one made from the player's own ROM (ps3/host/xmb_icon.cpp), so the repository
and the package hold no game artwork. Adds a PNG text chunk "conker-ps3: icon-from-rom"; standard library only.

    python3 tools/release/mark_icon.py IN.png OUT.png
"""
import struct
import sys
import zlib

MARK = b"conker-ps3\0icon-from-rom"


def main():
    data = open(sys.argv[1], "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        sys.exit(f"{sys.argv[1]}: not a PNG")
    after_ihdr = 8 + 8 + struct.unpack(">I", data[8:12])[0] + 4
    chunk = struct.pack(">I", len(MARK)) + b"tEXt" + MARK + struct.pack(">I", zlib.crc32(b"tEXt" + MARK) & 0xFFFFFFFF)
    open(sys.argv[2], "wb").write(data[:after_ihdr] + chunk + data[after_ihdr:])


if __name__ == "__main__":
    main()
