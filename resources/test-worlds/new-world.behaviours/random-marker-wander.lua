local promethium = require("promethium.v3")

-- Simulation time runs at 60 ticks per second.
local ARRIVAL_WAIT = 3 * 60
local RETRY_TIMER = "try_next_marker"
local WAIT_TIMER = "arrival_wait"

return {
  api_version = promethium.api_version,
  factory = function(configuration)
    return function(context)
      local last_arrival = nil
      while true do
        local remaining = {}
        local seen = {}
        for _, marker in ipairs(configuration.markers or {}) do
          if marker ~= last_arrival and not seen[marker] then
            remaining[#remaining + 1] = marker
            seen[marker] = true
          end
        end

        local arrived = false
        while #remaining > 0 and not arrived do
          local index = context.random_integer(1, #remaining)
          local destination = table.remove(remaining, index)
          local result = context.move_to(destination)
          if result.accepted then
            local event
            repeat
              event = wait()
            until event.type == "destination_reached" or event.type == "route_lost"
            if event.type == "destination_reached" then
              last_arrival = event.destination
              arrived = true
            end
          elseif #remaining > 0 then
            -- Only one movement intent is permitted per resume.
            context.set_timer(RETRY_TIMER, 1)
            local event
            repeat
              event = wait()
            until event.type == "timer_expired" and event.name == RETRY_TIMER
          end
        end

        if not arrived then
          return -- Exhaustion means idle, without polling or repeating failures.
        end
        context.set_timer(WAIT_TIMER, ARRIVAL_WAIT)
        local event
        repeat
          event = wait()
        until event.type == "timer_expired" and event.name == WAIT_TIMER
      end
    end
  end,
}
