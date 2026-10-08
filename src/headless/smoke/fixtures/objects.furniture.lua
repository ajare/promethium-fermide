-- Stable integration fixture, independent of editable teaching catalogues.
local function sit(agent, world, marker)

  world.claim()
end
local function stand(agent, world, marker)

  world.release()
end
return {
  api_version = 1,
  uuid = "1c639f04-1183-4e51-9385-86cce6b0f031",
  definitions = {
    {
      key = "desk", label = "Desk", sideRoutes = true,
      tiles = {
        {x = 0, y = 0, imageSet = "ObjectAtlas", image = "chair"},
        {x = 1, y = 0, imageSet = "ObjectAtlas", image = "chair"}
      },
      usablePoints = {{key = "seat", label = "Seat", x = 0.75}},
      vertices = {
        {key = "left", x = 0, external = true},
        {key = "right", x = 2, external = true},
        {key = "frontLeft", x = 0.25}, {key = "frontRight", x = 1.75},
        {key = "backLeft", x = 0.25}, {key = "backRight", x = 1.75},
        {key = "seat", x = 0.75, usablePoint = "seat"},
        {key = "disconnected", x = 0.75}
      },
      edges = {
        {from = "left", to = "frontLeft"},
        {from = "frontLeft", to = "frontRight", depthOffset = 0},
        {from = "frontRight", to = "right"},
        {from = "left", to = "backLeft"},
        {from = "backLeft", to = "backRight", depthOffset = 1},
        {from = "backRight", to = "right"},
        {from = "frontLeft", to = "seat", depthOffset = 0}
      }
    },
    {
      key = "chair", label = "Chair",
      tiles = {{x = 0, y = 0, imageSet = "ObjectAtlas", image = "chair"}},
      usablePoints = {{key = "seat", label = "Seat", x = 0.5}},
      use_pose = "sitting", finish_use_pose = "standing", use = sit, finish_use = stand
    },
    {
      key = "table", label = "Table without destinations",
      tiles = {{x = 0, y = 0, imageSet = "ObjectAtlas", image = "chair"}},
      usablePoints = {}
    }
  }
}
