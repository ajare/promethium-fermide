-- Native Lua catalogue; UUID, keys, artwork and routes are stable.
--
-- A width-1 chair falls out of the Furniture role convention as `chair-L0` rather
-- than being a special case (#1029): Furniture is the multi-cell kind, so all of
-- its tiles carry the role suffix.
local function sit(agent, world, marker)

  world.claim()
end
local function finish(agent, world, marker)

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
        {x = 0, y = 0, image = "chair-L0"},
      },
      usablePoints = {
        {key = "seat", label = "Seat", x = 0.5},
      },
      use_pose = "sitting", finish_use_pose = "standing", use = sit, finish_use = finish,
    },
  }
}
