-- Compact wheeled cleaning bot. It travels on level floors and can open
-- ordinary Doors, but cannot operate Buttons or Access panels.
return {
    api_version = 2,
    type_id = "CleaningBot",
    display_name = "Cleaning Bot",
    new = function()
        return {
            width = 0.4,
            standing_height = 0.15,
            reach = 0.1,
            walk_speed = 0.3,
            -- Required positive baselines; the Mobility profile forbids climbing.
            climb_speed = 0.1,
            stair_ascent_speed = 0.1,
            stair_descent_speed = 0.1,
            poses = { standing = { image_tile = "cleaning-bot-standing" } },
            automatic_poses = {
                room_movement = {{ pose = "standing", speed_ratio = 1 }},
                door_crossing = {{ pose = "standing", speed_ratio = 1 }},
            },
            mobility_profile = {
                staircase = "cannot_use", escalator = "cannot_use", stairwell = "cannot_use",
                ladder = "cannot_use", lift = "cannot_use", platform_lift = "cannot_use",
                shuttle = "cannot_use", door = "can_use", buttons = "cannot_use",
            },
        }
    end,
}
