#!/usr/bin/env python3
"""Bake Human stance transforms into dedicated atlas sprites, preserving old pixels."""
from pathlib import Path
from PIL import Image


def pack_human_poses(path: Path) -> None:
    source = Image.open(path).convert("RGBA")
    if source.width != 320 or source.height not in (800, 960):
        raise ValueError("Expected the bundled 320x800 or 320x960 ObjectAtlas")
    atlas = Image.new("RGBA", (320, 960))
    atlas.paste(source, (0, 0))
    standing = source.crop((83, 248, 109, 320))
    horizontal = standing.transpose(Image.Transpose.ROTATE_270)
    sprites = [
        (0, standing),
        (32, standing.resize((26, 43), Image.Resampling.NEAREST)),
        (64, horizontal),
        (144, standing.resize((26, 43), Image.Resampling.NEAREST)),
        (192, horizontal.resize((72, 8), Image.Resampling.NEAREST)),
    ]
    for x, sprite in sprites:
        atlas.paste(sprite, (x, 960 - sprite.height))
    robot_path = path.with_name("robot-standing.png")
    if robot_path.is_file():
        robot = Image.open(robot_path).convert("RGBA")
        if robot.size != (26, 72):
            raise ValueError("Expected a 26x72 Robot Standing tile")
        atlas.paste(robot, (288, 888))
    cleaning_bot_path = path.with_name("cleaning-bot-standing.png")
    if cleaning_bot_path.is_file():
        cleaning_bot = Image.open(cleaning_bot_path).convert("RGBA")
        if cleaning_bot.size != (26, 24):
            raise ValueError("Expected a 26x24 CleaningBot Standing tile")
        atlas.paste(cleaning_bot, (240, 912))
    atlas.save(path)


if __name__ == "__main__":
    pack_human_poses(Path(__file__).resolve().parents[1] / "resources/textures/objects.png")
