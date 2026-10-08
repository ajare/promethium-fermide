-- Bundled Human Agent type definition.
-- One .agent.lua source defines exactly one type object with a stable type ID,
-- a display name, and a new() constructor returning a fresh instance carrying
-- the complete physical baseline.
return {
    api_version = 1,
    type_id = "Human",
    display_name = "Human",
    new = function()
        return {
            width = 0.4,
            standing_height = (0.7 - 0.2) - 0.05,
            reach = 0.25,
            walk_speed = 0.5,
            climb_speed = 0.25,
            stair_ascent_speed = 0.35,
            stair_descent_speed = 0.45,
            sitting_height_ratio = 0.6,
            crouching_height_ratio = 0.6,
            crawling_height_ratio = 0.3,
            crawling_speed_ratio = 0.5,
        }
    end,
}
