-- Independent regression geometry; stable catalogue and point identities.
local catalogue = {
  api_version = 1,
  uuid = "7ad3b31e-c85b-43ac-90ba-359359359359",
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
        }},
      vertices = {{
          key = "approachLeft",
          x = 0,
          external = true,
        }, {
          key = "approachRight",
          x = 1,
          external = true,
        }, {
          key = "frontLeft",
          x = 0.125,
        }, {
          key = "frontRight",
          x = 0.875,
        }, {
          key = "backLeft",
          x = 0.125,
        }, {
          key = "backRight",
          x = 0.875,
        }, {
          key = "seat",
          x = 0.5,
          usablePoint = "seat",
        }},
      edges = {{
          from = "approachLeft",
          to = "frontLeft",
        }, {
          from = "frontLeft",
          to = "seat",
          depthOffset = 0,
        }, {
          from = "seat",
          to = "frontRight",
          depthOffset = 0,
        }, {
          from = "frontRight",
          to = "approachRight",
        }, {
          from = "approachLeft",
          to = "backLeft",
        }, {
          from = "backLeft",
          to = "backRight",
          depthOffset = 1,
        }, {
          from = "backRight",
          to = "approachRight",
        }},
    }, {
      key = "sofa",
      label = "Sofa",
      sideRoutes = true,
      tiles = {{
          x = 0,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "sofa-left",
        }, {
          x = 1,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "sofa-right",
        }},
      usablePoints = {{
          key = "left",
          label = "Left seat",
          x = 0.5,
          blocksPathing = false,
        }, {
          key = "right",
          label = "Right seat",
          x = 1.5,
          blocksPathing = false,
        }},
      vertices = {{
          key = "left",
          x = 0,
          external = true,
        }, {
          key = "frontLeft",
          x = 0.25,
        }, {
          key = "frontRight",
          x = 1.75,
        }, {
          key = "backLeft",
          x = 0.25,
        }, {
          key = "backRight",
          x = 1.75,
        }, {
          key = "right",
          x = 2,
          external = true,
        }, {
          key = "leftSeat",
          x = 0.5,
          usablePoint = "left",
        }, {
          key = "rightSeat",
          x = 1.5,
          usablePoint = "right",
        }},
      edges = {{
          from = "left",
          to = "frontLeft",
        }, {
          from = "left",
          to = "backLeft",
        }, {
          from = "frontLeft",
          to = "leftSeat",
          depthOffset = 0,
        }, {
          from = "leftSeat",
          to = "rightSeat",
          depthOffset = 0,
        }, {
          from = "rightSeat",
          to = "frontRight",
          depthOffset = 0,
        }, {
          from = "frontRight",
          to = "right",
        }, {
          from = "backLeft",
          to = "backRight",
          depthOffset = 1,
        }, {
          from = "backRight",
          to = "right",
        }},
    }, {
      key = "bed",
      label = "Bed",
      sideRoutes = true,
      tiles = {{
          x = 0,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "bed-left",
        }, {
          x = 1,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "bed-right",
        }},
      usablePoints = {{
          key = "middle",
          label = "Middle",
          x = 1,
          blocksPathing = false,
        }},
      vertices = {{
          key = "left",
          x = 0,
          external = true,
        }, {
          key = "right",
          x = 2,
          external = true,
        }, {
          key = "frontLeft",
          x = 0.125,
        }, {
          key = "frontRight",
          x = 1.875,
        }, {
          key = "backLeft",
          x = 0.125,
        }, {
          key = "backRight",
          x = 1.875,
        }, {
          key = "middle",
          x = 1,
          usablePoint = "middle",
        }},
      edges = {{
          from = "left",
          to = "frontLeft",
        }, {
          from = "frontLeft",
          to = "middle",
          depthOffset = 0,
        }, {
          from = "middle",
          to = "frontRight",
          depthOffset = 0,
        }, {
          from = "frontRight",
          to = "right",
        }, {
          from = "left",
          to = "backLeft",
        }, {
          from = "backLeft",
          to = "backRight",
          depthOffset = 1,
        }, {
          from = "backRight",
          to = "right",
        }},
    }, {
      key = "desk",
      label = "Desk",
      sideRoutes = true,
      tiles = {{
          x = 0,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "desk-left",
        }, {
          x = 1,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "desk-right",
        }},
      usablePoints = {},
      vertices = {{
          key = "left",
          x = 0,
          external = true,
        }, {
          key = "right",
          x = 2,
          external = true,
        }, {
          key = "frontLeft",
          x = 0.25,
        }, {
          key = "frontRight",
          x = 1.75,
        }, {
          key = "backLeft",
          x = 0.25,
        }, {
          key = "backRight",
          x = 1.75,
        }},
      edges = {{
          from = "left",
          to = "frontLeft",
        }, {
          from = "frontLeft",
          to = "frontRight",
          depthOffset = 0,
        }, {
          from = "frontRight",
          to = "right",
        }, {
          from = "left",
          to = "backLeft",
        }, {
          from = "backLeft",
          to = "backRight",
          depthOffset = 1,
        }, {
          from = "backRight",
          to = "right",
        }},
    }},
}
-- Chair and desk side depths extend to the outside boundaries.
local chair = catalogue.definitions[1]
chair.edges[1].depthOffset = 0
chair.edges[4].depthOffset = 0
chair.edges[5].depthOffset = 1
chair.edges[7].depthOffset = 1
table.insert(chair.vertices, {key='floorLeft', x=0})
table.insert(chair.vertices, {key='floorRight', x=1})
table.insert(chair.edges, {from='floorLeft', to='approachLeft'})
table.insert(chair.edges, {from='floorRight', to='approachRight'})
catalogue.definitions[4].edges[4].depthOffset = 1
catalogue.definitions[4].edges[6].depthOffset = 1
catalogue.definitions[1].use = function(agent, world, marker)
  world.set_pose('sitting')
  world.claim()
end
catalogue.definitions[1].finish_use = function(agent, world, marker)
  world.set_pose('standing')
  world.release()
end
catalogue.definitions[2].use = function(agent, world, marker)
  world.set_pose('sitting')
  world.claim()
end
catalogue.definitions[2].finish_use = function(agent, world, marker)
  world.set_pose('standing')
  world.release()
end
catalogue.definitions[3].use = function(agent, world, marker)
  world.set_pose('lying')
  world.claim()
end
catalogue.definitions[3].finish_use = function(agent, world, marker)
  world.set_pose('standing')
  world.release()
end
return catalogue
