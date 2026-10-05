-- Native Lua catalogue; UUID, keys, artwork and routes are stable.
local function sit(agent, world, marker)
  world.set_pose("sitting")
  world.claim()
end
local function finish(agent, world, marker)
  world.set_pose("standing")
  world.release()
end
return {
  api_version = 1,
  uuid = "8ac1c0cd-3744-4f1e-9c42-1a7a41404514",
  definitions = {
    {
      key = "desk",
      label = "Desk with front and back routes",
      sideRoutes = true,
      tiles = {
        {x = 0, y = 0, imageSet = "ObjectAtlas", image = "chair"},
        {x = 1, y = 0, imageSet = "ObjectAtlas", image = "chair"},
      },
      usablePoints = {
        {key = "seat", label = "Seat", x = 0.75},
      },
      vertices = {
        {key = "left", x = 0, external = true},
        {key = "right", x = 2, external = true},
        {key = "frontLeft", x = 0.25},
        {key = "frontRight", x = 1.75},
        {key = "backLeft", x = 0.25},
        {key = "backRight", x = 1.75},
        {key = "seat", x = 0.75, usablePoint = "seat"},
        {key = "disconnected", x = 0.75},
      },
      edges = {
        {from = "left", to = "frontLeft"},
        {from = "frontLeft", to = "frontRight", depthOffset = 0},
        {from = "frontRight", to = "right"},
        {from = "left", to = "backLeft"},
        {from = "backLeft", to = "backRight", depthOffset = 1},
        {from = "backRight", to = "right"},
        {from = "frontLeft", to = "seat", depthOffset = 0},
      },
    },
    {
      key = "chair",
      label = "Attaching chair",
      tiles = {
        {x = 0, y = 0, imageSet = "ObjectAtlas", image = "chair"},
      },
      usablePoints = {
        {key = "seat", label = "Seat", x = 0.5},
      },
      vertices = {
        {key = "approach", x = 0, external = true},
        {key = "seat", x = 0.5, usablePoint = "seat"},
      },
      edges = {
        {from = "approach", to = "seat", depthOffset = 1},
      },
      use = sit, finish_use = finish,
    },
  }
}
