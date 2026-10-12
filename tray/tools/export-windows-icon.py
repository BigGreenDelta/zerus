#!/usr/bin/env python3
"""Regenerate the Windows application icon from the checked-in macOS icon.

hgs-zerus.ico is embedded into hgs-tray.exe (packaging/hgs-tray.rc). Normal
builds use the checked-in file; run this only after the artwork changes:

    python3 tray/tools/export-windows-icon.py

Requires Pillow.
"""
from pathlib import Path

from PIL import Image

ICONS = Path(__file__).resolve().parent.parent / "resources" / "icons"
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def main() -> None:
    # Pillow opens the largest image of the ICNS file (256 px).
    image = Image.open(ICONS / "hgs-zerus-swarm.icns").convert("RGBA")
    # macOS draws its tile inside a transparent margin; Windows icons use the canvas.
    box = image.getbbox()
    if box:
        image = image.crop(box)
    side = max(image.size)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(image, ((side - image.size[0]) // 2, (side - image.size[1]) // 2))
    canvas = canvas.resize((SIZES[-1], SIZES[-1]), Image.LANCZOS)
    canvas.save(ICONS / "hgs-zerus.ico", sizes=[(size, size) for size in SIZES])


if __name__ == "__main__":
    main()
