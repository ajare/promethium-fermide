-- Scout Agent type fixture (#505).
-- A second, distinctly shaped and speeded Agent type defined entirely in Lua.
-- It needs no C++ subtype or factory change: the host validates and freezes
-- this baseline and every simulation consumer reads the frozen values.
return {
    api_version = 1,
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
            sitting_height_ratio = 0.5,
            crouching_height_ratio = 0.5,
            crawling_height_ratio = 0.25,
            crawling_speed_ratio = 0.75,
        }
    end,
}
