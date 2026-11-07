-- Independent regression geometry; stable catalogue and point identities.
local catalogue = {
  api_version = 1,
  uuid = "9a486bac-52cc-4c45-a190-3a317be86536",
  definitions = {{
      key = "outer",
      label = "Wide replacement",
      sideRoutes = true,
      tiles = {{
          x = 0,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "chair",
        }, {
          x = 1,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "chair",
        }, {
          x = 2,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "chair",
        }, {
          x = 3,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "chair",
        }},
      usablePoints = {{
          key = "seat",
          label = "Seat",
          x = 0.75,
        }},
      vertices = {{
          key = "left",
          x = 0,
          external = true,
        }, {
          key = "frontLeft",
          x = 0.25,
        }, {
          key = "middle",
          x = 2,
        }, {
          key = "frontRight",
          x = 3.75,
        }, {
          key = "right",
          x = 4,
          external = true,
        }, {
          key = "seat",
          x = 0.75,
          usablePoint = "seat",
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
          from = "frontLeft",
          to = "seat",
          depthOffset = 0,
        }},
    }, {
      key = "inner",
      label = "Narrow replacement",
      sideRoutes = true,
      tiles = {{
          x = 0,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "chair",
        }, {
          x = 1,
          y = 0,
          imageSet = "ObjectAtlas",
          image = "chair",
        }},
      usablePoints = {{
          key = "seat",
          label = "Seat",
          x = 0.75,
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
          key = "right",
          x = 2,
          external = true,
        }, {
          key = "seat",
          x = 0.75,
          usablePoint = "seat",
        }},
      edges = {{
          from = "left",
          to = "frontLeft",
        }, {
          from = "frontLeft",
          to = "frontRight",
          depthOffset = -1,
        }, {
          from = "frontRight",
          to = "right",
        }, {
          from = "left",
          to = "seat",
          depthOffset = -1,
        }, {
          from = "left",
          to = "right",
          depthOffset = -1,
        }},
    }},
}
return catalogue
