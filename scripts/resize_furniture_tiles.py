#!/usr/bin/env python3
"""Scale bundled Furniture artwork to human proportions (requires Pillow).

A Human is 0.45 World units tall, or 72
pixels at 160 pixels/World unit. Chair/sofa backs are about 61%/47% of
that height; the desk surface is about 44%. Seats are roughly 25%.
Keep full 64x160 Image-set cells and catalogue topology unchanged.
"""
from pathlib import Path
from PIL import Image


def resize_furniture(path):
    atlas = Image.open(path).convert("RGBA")
    if atlas.width != 320 or atlas.height < 640:
        raise ValueError("Expected the bundled ObjectAtlas with at least four rows")
    # Resize joined sofa/desk tiles together to preserve the centre seam.
    for name, box, size in (
        ("chair", (256, 160, 320, 320), (21, 44)),
        ("sofa", (0, 480, 128, 640), (62, 34)),
        ("desk", (128, 480, 256, 640), (64, 32)),
    ):
        cell = atlas.crop(box)
        bounds = cell.getbbox()
        if bounds is None:
            raise ValueError(f"Missing {name} artwork")
        sprite = cell.crop(bounds).resize(size, Image.Resampling.LANCZOS)
        # Remove nearly invisible filter fringes, including transparent RGB.
        pixels = sprite.load()
        for y in range(sprite.height):
            for x in range(sprite.width):
                if pixels[x, y][3] < 12:
                    pixels[x, y] = (0, 0, 0, 0)
        result = Image.new("RGBA", cell.size)
        result.paste(sprite, ((cell.width - size[0]) // 2, cell.height - size[1]))
        atlas.paste(result, box[:2])
    atlas.save(path)


if __name__ == "__main__":
    resize_furniture(Path(__file__).resolve().parents[1] / "resources/textures/objects.png")
