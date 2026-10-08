-- Independent regression geometry; stable catalogue and point identities.
local catalogue = {
  api_version = 1,
  uuid = "e78a6c36-7902-4abc-9c90-1876058b32f3",
  definitions = {{
      key = "chair",
      label = "Chair",
      sideRoutes = true,
      tiles = {{
          x = 0,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "chair",
        }},
      usablePoints = {{
          key = "seat",
          label = "Seat",
          x = 0.5,
          blocksPathing = false,
        }, {
          key = "plain",
          label = "Plain",
          x = 0.75,
          blocksPathing = false,
        }},
      vertices = {{
          key = "left",
          x = 0,
          external = true,
        }, {
          key = "seat",
          x = 0.5,
          usablePoint = "seat",
        }, {
          key = "plain",
          x = 0.75,
          usablePoint = "plain",
        }, {
          key = "right",
          x = 1,
          external = true,
        }},
      edges = {{
          from = "left",
          to = "seat",
        }, {
          from = "seat",
          to = "plain",
        }, {
          from = "plain",
          to = "right",
        }},
    }},
}
catalogue.definitions[1].use_pose = "sitting"
catalogue.definitions[1].finish_use_pose = "standing"
catalogue.definitions[1].use = function(agent, world, marker)
  if marker.usable_point == 'plain' then return end

  world.claim()
end
catalogue.definitions[1].finish_use = function(agent, world, marker)

  world.release()
end
return catalogue
