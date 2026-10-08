-- Bundled Robot Agent type: Human physical and mobility baselines,
-- but Standing is its only supported pose.
return {
    api_version = 2,
    type_id = "Robot",
    display_name = "Robot",
    new = function()
        return {
            width = 0.4,
            standing_height = (0.7 - 0.2) - 0.05,
            reach = 0.25,
            walk_speed = 0.5,
            climb_speed = 0.25,
            stair_ascent_speed = 0.35,
            stair_descent_speed = 0.45,
            poses = { standing = { image_tile = "agent" } },
            automatic_poses = {
                room_movement = {{ pose = "standing", speed_ratio = 1 }},
                door_crossing = {{ pose = "standing", speed_ratio = 1 }},
            },
            mobility_profile = {
                staircase = "can_use", escalator = "can_use", stairwell = "can_use",
                ladder = "can_use", lift = "can_use", platform_lift = "can_use",
                shuttle = "can_use", door = "can_use", buttons = "can_use",
            },
        }
    end,
}
