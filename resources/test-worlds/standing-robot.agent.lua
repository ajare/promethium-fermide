-- Standing-only regression type for Agent-type API v2 (#521).
return {
    api_version = 2,
    type_id = "StandingRobot",
    display_name = "Standing Robot",
    new = function()
        return {
            width = 0.3, standing_height = 0.4, reach = 0.25,
            walk_speed = 0.5, climb_speed = 0.25,
            stair_ascent_speed = 0.35, stair_descent_speed = 0.45,
            poses = { standing = {} },
            automatic_poses = {
                room_movement = {{ pose = "standing", speed_ratio = 1 }},
                door_crossing = {{ pose = "standing", speed_ratio = 1 }},
            },
            mobility_profile = {
                staircase = "can_use", escalator = "can_use", stairwell = "can_use",
                ladder = "can_use", lift = "can_use", platform_lift = "can_use",
                shuttle = "can_use", door = "can_use", buttons = "can_use",
            },
            private_state = {},
        }
    end,
}
