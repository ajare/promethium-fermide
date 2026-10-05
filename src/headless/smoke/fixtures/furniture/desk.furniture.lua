-- Independent regression geometry; stable catalogue and point identities.
local catalogue = {
  api_version = 1,
  uuid = "1c639f04-1183-4e51-9385-86cce6b0f031",
  definitions = {{
      key = "desk",
      label = "Desk with front and back routes",
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
        }, {
          key = "seat",
          x = 0.75,
          usablePoint = "seat",
        }, {
          key = "disconnected",
          x = 0.75,
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
        }, {
          from = "frontLeft",
          to = "seat",
          depthOffset = 0,
        }},
    }},
}
return catalogue
