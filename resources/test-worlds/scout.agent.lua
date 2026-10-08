-- Scout Agent type fixture (#505).
-- A second, distinctly shaped and speeded Agent type defined entirely in Lua.
-- It needs no C++ subtype or factory change: the host validates and freezes
-- this baseline and every simulation consumer reads the frozen values.
return {
    api_version = 2,
    type_id = "Scout",
    display_name = "Scout",
    new = function()
        return {
            width = 0.3,
            standing_height = 0.35,
            reach = 0.4,
            walk_speed = 0.9,
            climb_speed = 0.5,
            stair_ascent_speed = 0.6,
            stair_descent_speed = 0.7,
            poses = {
                standing = { image_tile = "agent" },
                sitting = { image_tile = "agent", height_ratio = 0.5 },
                lying = { image_tile = "agent", height_ratio = 0.5 / 0.75, width_ratio = 0.75 / 0.5 },
                crouching = { image_tile = "agent", height_ratio = 0.5 },
                crawling = { image_tile = "agent", height_ratio = 0.25 },
            },
            automatic_poses = {
                room_movement = {
                    { pose = "standing", speed_ratio = 1 },
                    { pose = "crouching", speed_ratio = 1 },
                    { pose = "crawling", speed_ratio = 1 },
                },
                door_crossing = {
                    { pose = "standing", speed_ratio = 1 },
                    { pose = "crouching", speed_ratio = 1 },
                    { pose = "crawling", speed_ratio = 0.75 },
                },
            },
            mobility_profile = {
                staircase = "can_use", escalator = "can_use", stairwell = "can_use",
                ladder = "can_use", lift = "can_use", platform_lift = "can_use",
                shuttle = "can_use", door = "can_use", buttons = "can_use",
            },
        }
    end,
}
