#!/usr/bin/env python3
"""Regenerate BoothWindow frame/shutter pixels without touching other atlas cells."""
from pathlib import Path
from PIL import Image, ImageDraw

path = Path(__file__).resolve().parents[1] / "resources/textures/objects.png"
image = Image.open(path).convert("RGBA")
if image.size != (320, 640):
    raise ValueError("Expected the bundled 320x640 ObjectAtlas")
draw = ImageDraw.Draw(image)
for y, closed in ((560, False), (496, True)):
    draw.rectangle((262, y, 313, y + 47), fill=(0, 0, 0, 0))
    draw.rectangle((262, y, 313, y + 47), outline=(38, 48, 58, 255), width=3)
    draw.rectangle((265, y + 3, 310, y + 44), outline=(148, 162, 170, 255), width=2)
    if closed:
        draw.rectangle((267, y + 5, 308, y + 42), fill=(90, 106, 116, 255))
        for row in range(y + 9, y + 42, 6):
            draw.line((267, row, 308, row), fill=(52, 68, 78, 255))
image.save(path)
