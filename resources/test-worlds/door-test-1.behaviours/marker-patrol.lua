local promethium = require("promethium.v3")

local WAIT_TICKS = 5 * 60

return {
  api_version = promethium.api_version,
  factory = function(configuration)
    return function(context)
      local heading_to_first_marker = true
      while true do
        local destination = heading_to_first_marker
          and configuration.first_marker
          or configuration.second_marker
        local result = context.move_to(destination)
        if not result.accepted then
          error("could not move to the next Marker: " .. result.status)
        end

        local event
        repeat
          event = wait()
        until event.type == "destination_reached"

        heading_to_first_marker = not heading_to_first_marker
        context.set_timer("marker_wait", WAIT_TICKS)
        repeat
          event = wait()
        until event.type == "timer_expired" and event.name == "marker_wait"
      end
    end
  end,
}
