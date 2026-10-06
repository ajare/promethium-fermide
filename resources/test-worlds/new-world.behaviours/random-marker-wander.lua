local promethium = require("promethium.v2")

-- Simulation time runs at 60 ticks per second.
local ARRIVAL_WAIT = 3 * 60
local RETRY_TIMER = "try_next_marker"
local WAIT_TIMER = "arrival_wait"

return {
  api_version = promethium.api_version,
  factory = function(configuration)
    local remaining = {}
    local last_arrival = nil
    local travelling = false

    local function try_next(context)
      travelling = false
      if #remaining == 0 then
        return -- No possible new destination: remain idle, without polling.
      end

      local index = context.random_integer(1, #remaining)
      local destination = table.remove(remaining, index)
      local result = context.move_to(destination)
      if result.accepted then
        travelling = true
      elseif #remaining > 0 then
        -- The host permits only one movement command per callback.
        context.set_timer(RETRY_TIMER, 1)
      end
    end

    local function begin_trip(context)
      remaining = {}
      local seen = {}
      for _, marker in ipairs(configuration.markers or {}) do
        if marker ~= last_arrival and not seen[marker] then
          remaining[#remaining + 1] = marker
          seen[marker] = true
        end
      end
      try_next(context)
    end

    return {
      on_start = function(context)
        begin_trip(context)
      end,

      on_event = function(event, context)
        if event.type == "destination_reached" and travelling then
          travelling = false
          last_arrival = event.destination
          context.set_timer(WAIT_TIMER, ARRIVAL_WAIT)
        end
      end,

      on_timer = function(name, context)
        if name == WAIT_TIMER then
          begin_trip(context)
        elseif name == RETRY_TIMER then
          try_next(context)
        end
      end,

      on_route_lost = function(destination, reason, context)
        -- Try the remaining Markers without replacement. Exhaustion means idle.
        try_next(context)
      end,
    }
  end,
}
