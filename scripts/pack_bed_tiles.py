#!/usr/bin/env python3
"""Add two bottom-anchored Bed tiles, preserving all existing ObjectAtlas pixels."""
from pathlib import Path
from PIL import Image, ImageDraw

path = Path(__file__).resolve().parents[1] / "resources/textures/objects.png"
source = Image.open(path).convert("RGBA")
if source.width != 320 or source.height not in (640, 800):
    raise ValueError("Expected the bundled ObjectAtlas")
atlas = Image.new("RGBA", (320, 800))
atlas.paste(source, (0, 0))
# Joined tiles keep a seamless mattress/blanket across the central usable point.
bed = Image.new("RGBA", (128, 160))
draw = ImageDraw.Draw(bed)
draw.rectangle((5, 142, 11, 159), fill="#34434d")
draw.rectangle((116, 142, 122, 159), fill="#34434d")
draw.rounded_rectangle((2, 126, 125, 148), radius=4, fill="#526675", outline="#263640", width=2)
draw.rounded_rectangle((5, 117, 122, 135), radius=5, fill="#e3e5df", outline="#85979e", width=2)
draw.rounded_rectangle((8, 111, 28, 123), radius=4, fill="#faf7ec", outline="#b4b9b4", width=2)
draw.rounded_rectangle((29, 114, 121, 134), radius=3, fill="#648d9c", outline="#344f61", width=2)
draw.line((34, 116, 34, 132), fill="#a1bbc1", width=3)
draw.rectangle((1, 98, 5, 149), fill="#526675", outline="#263640", width=1)
draw.rectangle((123, 116, 127, 149), fill="#526675", outline="#263640", width=1)
# Mirror the joined artwork, not each cell separately: headboard/pillow on the right.
bed = bed.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
atlas.paste(bed, (0, 640))
atlas.save(path)
