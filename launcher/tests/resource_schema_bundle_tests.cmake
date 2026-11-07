function(run_command expected_result description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE actual_result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT actual_result EQUAL expected_result)
        message(FATAL_ERROR
            "${description}: command returned ${actual_result}, expected ${expected_result}\n"
            "stdout:\n${output}\nstderr:\n${error}")
    endif()
endfunction()

run_command(0 "bundle verification"
    "${RESOURCE_MANAGER}" --verify-schemas --ini "${INI}")
run_command(0 "BooleanWorld Resources.yaml"
    "${VALIDATOR}" "${BUNDLE}" "${APPLICATION_MANIFEST}")
run_command(0 "valid application Resource Types"
    "${VALIDATOR}" "${BUNDLE}"
    "${FIXTURE_DIRECTORY}/valid-application-resources.yaml")
run_command(0 "legacy LuaScript Params"
    "${VALIDATOR}" "${BUNDLE}"
    "${FIXTURE_DIRECTORY}/valid-legacy-lua-script.yaml")

foreach(invalid_fixture
        invalid-map-default.yaml
        invalid-map-tiled.yaml
        invalid-map-boolean-world.yaml
        invalid-proto-entity.yaml
        invalid-lua-script.yaml
        invalid-lua-script-mixed-vars.yaml
        invalid-lua-script-number-var.yaml
        invalid-proc-material.yaml
        invalid-embossing-catalog.yaml
        invalid-acoustic-catalog.yaml)
    run_command(4 "${invalid_fixture}"
        "${VALIDATOR}" "${BUNDLE}"
        "${FIXTURE_DIRECTORY}/${invalid_fixture}")
endforeach()
