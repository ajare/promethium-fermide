local promethium = require("promethium.v3")

return {
  api_version = promethium.api_version,
  factory = function(configuration)
    return function(context)
      local result = context.move_to(configuration.goal)
      if not result.accepted then
        error("laboratory journey refused: " .. result.status)
      end
      while true do
        local event = wait()
        if event.type == "destination_reached" or event.type == "route_lost" then
          return
        end
      end
    end
  end,
}
