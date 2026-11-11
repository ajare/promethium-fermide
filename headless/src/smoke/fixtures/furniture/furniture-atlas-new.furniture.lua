-- Role-named Furniture tiles for the FurnitureAtlas checks (#1016).
--
-- Every tile here is named `<piece>-<role><level>`: the role is `L`, `M` or
-- `R` - the left cap, a middle cell, the right cap - and the level is the
-- height level within the piece's footprint. The anchor is the footprint's
-- bottom-left, so every offset is non-negative and one digit per axis names
-- any of them.
--
-- `sofaWide` is the point of the whole convention: it is four cells wide
-- and needs no fourth tile. It draws `sofa-M0` twice, because the middle
-- tile is named for what it is for and not for where one particular width
-- happened to put it.
--
-- `legacySofa` and `bareChair` are the old artwork's vocabulary. They are
-- retained so the checks can prove the loader leaves names that predate the
-- convention alone instead of refusing every catalogue written before it.
local sofa = {
  key = "sofa",
  label = "Sofa",
  tiles = {
    {x = 0, y = 0, image = "sofa-L0"},
    {x = 1, y = 0, image = "sofa-M0"},
    {x = 2, y = 0, image = "sofa-R0"},
  },
  usablePoints = {},
}
local sofaWide = {
  key = "sofaWide",
  label = "Wide sofa",
  tiles = {
    {x = 0, y = 0, image = "sofa-L0"},
    {x = 1, y = 0, image = "sofa-M0"},
    {x = 2, y = 0, image = "sofa-M0"},
    {x = 3, y = 0, image = "sofa-R0"},
  },
  usablePoints = {},
}
local chair = {
  key = "chair",
  label = "Chair",
  tiles = {
    {x = 0, y = 0, image = "chair-L0"},
  },
  usablePoints = {},
}
-- The absolute `-XY` escape hatch: an offset from the anchor rather than a
-- role, for the irregular footprint a role vocabulary cannot name. These
-- three tiles are deliberately not in FurnitureAtlas - the sheet holds
-- the role tiles and the bare chair cell and nothing else - because this
-- definition exists to exercise the loader, never the renderer.
local irregularDesk = {
  key = "irregularDesk",
  label = "Irregular desk",
  tiles = {
    {x = 0, y = 0, image = "desk-00"},
    {x = 1, y = 0, image = "desk-10"},
    {x = 2, y = 0, image = "desk-20"},
  },
  usablePoints = {},
}
local legacySofa = {
  key = "legacySofa",
  label = "Legacy sofa",
  tiles = {
    {x = 0, y = 0, image = "sofa-left"},
    {x = 1, y = 0, image = "sofa-right"},
  },
  usablePoints = {},
}
-- A single-cell object keeps its bare name: no suffix carrying no
-- information.
local bareChair = {
  key = "bareChair",
  label = "Bare chair",
  tiles = {
    {x = 0, y = 0, image = "chair"},
  },
  usablePoints = {},
}
local catalogue = {
  api_version = 1,
  uuid = "3d9e4a12-7b6c-4d5e-8f01-23456789abcd",
  definitions = {sofa, sofaWide, chair, irregularDesk, legacySofa, bareChair},
}
return catalogue
