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
  uuid = "e78a6c36-7902-4abc-9c90-1876058b32f3",
  definitions = {
    {
      key = "chair",
      label = "Chair",
      tiles = {
        {x = 0, y = 0, imageSet = "ObjectAtlas", image = "chair"},
      },
      usablePoints = {
        {key = "seat", label = "Seat", x = 0.5},
      },
      use = sit, finish_use = finish,
    },
  }
}
