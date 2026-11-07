# Inventories are shared with the unchanged exhaustive contract below this include.
# No second list of registered names is maintained for the development lane.
if(NOT LANE MATCHES "^(cli|concurrency)$")
    message(FATAL_ERROR "Unknown contract lane: ${LANE}")
endif()
if(NOT DEFINED RUN_TIMEOUT)
    set(RUN_TIMEOUT 240)
endif()
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../../../scripts/tests/smoke_lane.py"
    --binary "${lane_binary}" --module "${lane_module}" --probe "${PROBE}"
    --lane "${LANE}" --timeout "${RUN_TIMEOUT}" ${lane_names}
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err
    TIMEOUT 900)
file(REMOVE_RECURSE "${work}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${lane_module} ${LANE}: ${result}\n${out}\n${err}")
endif()
message("${out}")
