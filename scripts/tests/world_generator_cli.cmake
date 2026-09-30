# Integration checks for publication safety and CLI errors. All outputs are fixtures.
if(NOT DEFINED GENERATOR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "GENERATOR and FIXTURE_DIR are required")
endif()
file(MAKE_DIRECTORY "${FIXTURE_DIR}")
set(world "${FIXTURE_DIR}/new-world.world")
set(manifest "${FIXTURE_DIR}/new-world.behaviours/behaviours.yaml")

execute_process(COMMAND "${GENERATOR}" --seed 42 --force --output "${world}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Fixture generation failed: ${output}\n${errors}")
endif()
file(SHA256 "${world}" world_before)
file(SHA256 "${manifest}" package_before)
file(SHA256 "${FIXTURE_DIR}/test.tags.yaml" tags_before)

function(expect_refusal)
    execute_process(COMMAND "${GENERATOR}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(result EQUAL 0)
        message(FATAL_ERROR "Command unexpectedly succeeded: ${ARGN}\n${output}")
    endif()
    file(SHA256 "${world}" world_after)
    file(SHA256 "${manifest}" package_after)
    file(SHA256 "${FIXTURE_DIR}/test.tags.yaml" tags_after)
    if(NOT world_after STREQUAL world_before OR NOT package_after STREQUAL package_before
        OR NOT tags_after STREQUAL tags_before)
        message(FATAL_ERROR "A refused generation modified existing output")
    endif()
endfunction()

expect_refusal(--seed 42 --output "${world}")
expect_refusal(--seed -1 --force --output "${world}")
expect_refusal(--seed 18446744073709551616 --force --output "${world}")
expect_refusal(--seed 42 --force --output "${world}.yaml")
expect_refusal(--unknown value --force --output "${world}")

# Failure after geometry/population generation must not replace a valid old World/package.
file(WRITE "${FIXTURE_DIR}/invalid.lua" "this is not a valid Lua module!\n")
expect_refusal(--seed 42 --force --output "${world}"
    --behaviour "${FIXTURE_DIR}/invalid.lua")

execute_process(COMMAND "${GENERATOR}" --seed 2026 --force --output "${world}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Explicit replacement failed: ${output}\n${errors}")
endif()
file(SHA256 "${world}" replacement)
if(replacement STREQUAL world_before)
    message(FATAL_ERROR "Explicit replacement did not change the generated World")
endif()
file(GLOB leftovers "${FIXTURE_DIR}/.world-generator-*")
if(leftovers)
    message(FATAL_ERROR "Staging directories leaked: ${leftovers}")
endif()
message(STATUS "World generator CLI refusal and replacement checks passed")
