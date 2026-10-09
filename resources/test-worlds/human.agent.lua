-- Bundled Human Agent type definition.
-- One .agent.lua source defines exactly one type object with a stable type ID,
-- a display name, and a new() constructor returning a fresh instance carrying
-- the complete physical baseline.
return {
    api_version = 2,
    type_id = "Human",
    display_name = "Human",
    new = function()
        return {
            width = 0.4,
            standing_height = (0.7 - 0.2) - 0.05,
            object_usage = "arms",
            object_usage_distance = 0.25,
            walk_speed = 0.5,
            climb_speed = 0.25,
            stair_ascent_speed = 0.35,
            stair_descent_speed = 0.45,
            poses = {
                standing = { image_tile = "human-standing" },
                sitting = { image_tile = "human-sitting", height_ratio = 0.6 },
                lying = { image_tile = "human-lying", height_ratio = 26 / 72, width_ratio = 72 / 26 },
                crouching = { image_tile = "human-crouching", height_ratio = 0.6 },
                crawling = { image_tile = "human-crawling", height_ratio = 0.3 },
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
                    { pose = "crawling", speed_ratio = 0.5 },
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
