-- Native Lua catalogue; UUID, keys, artwork and routes are stable.
--
-- The `sofa` piece wears the Furniture role convention (#1029). The `larger`
-- piece does not: its footprint reaches a negative offset from its anchor and a
-- second height level with no left cap at that level, so neither the role
-- vocabulary nor the `-XY` escape hatch can name it as placed. Its tiles speak
-- the bare `chair` name instead, which is the one position-free name
-- `resources/textures/furniture-new.tileset.yaml` carries art for. Widening the
-- sheet by that cell closed the gap this piece used to be recorded under
-- `uncovered_footprints` for (#1024), so the record is gone and the piece draws.
local function sit(agent, world, marker)

  world.claim()
end
local function finish(agent, world, marker)

  world.release()
end
return {
  api_version = 1,
  uuid = "8b709c26-9a12-4dd7-aabb-548579102f34",
  definitions = {
    {
      key = "sofa",
      label = "Two-seat sofa",
      tiles = {
        {x = 0, y = 0, image = "sofa-L0"},
        {x = 1, y = 0, image = "sofa-R0"},
      },
      usablePoints = {
        {key = "left", label = "Left seat", x = 0.25, y = 0},
        {key = "right", label = "Right seat", x = 1.625, y = 0},
      },
      use_pose = "sitting", finish_use_pose = "standing", use = sit, finish_use = finish,
    },
    {
      key = "larger",
      label = "Larger layout",
      tiles = {
        {x = -1, y = 0, image = "chair"},
        {x = 1, y = 0, image = "chair"},
        {x = 0, y = 1, image = "chair"},
      },
      usablePoints = {
        {key = "left", label = "Left point", x = -0.75, y = 0},
        {key = "middle", label = "Middle point", x = 0.375, y = 0},
        {key = "right", label = "Right point", x = 1.75, y = 0},
      },
    },
  }
}
