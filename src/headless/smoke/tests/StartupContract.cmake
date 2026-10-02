# Public Startup CLI from an empty directory outside the source/build trees.
if(WIN32)
    set(temp "$ENV{TEMP}")
else()
    set(temp "/tmp")
endif()
string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef suffix)
set(work "${temp}/pf startup contract ${suffix}")
file(MAKE_DIRECTORY "${work}")

function(invoke status expected)
    execute_process(COMMAND "${STARTUP}" ${ARGN}
        WORKING_DIRECTORY "${work}" RESULT_VARIABLE result
        OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 40)
    if(NOT "${result}" STREQUAL "${status}")
        message(FATAL_ERROR "${ARGN}: expected ${status}, got ${result}\n${out}\n${err}")
    endif()
    if(status EQUAL 2)
        if(NOT out STREQUAL "" OR NOT err MATCHES "^ERROR startup:")
            message(FATAL_ERROR "Unexpected misuse output: ${out} / ${err}")
        endif()
    elseif(NOT err STREQUAL "" OR NOT out MATCHES "${expected}")
        message(FATAL_ERROR "Unexpected result: ${out} / ${err}")
    endif()
endfunction()

invoke(0 "^graphicsInitializationFailure\n$" --list)
invoke(0 "^PASS startup graphicsInitializationFailure\nSUMMARY startup pass=1 fail=0 skip=0\n$")
invoke(0 "^PASS startup graphicsInitializationFailure\nSUMMARY startup pass=1 fail=0 skip=0\n$"
    --check graphicsInitializationFailure)
foreach(arguments IN ITEMS "--bogus" "--check" "--check;absent" "--list;extra"
        "--check;graphicsInitializationFailure;extra")
    invoke(2 "" ${arguments})
endforeach()

# The editor is a required child product. An explicit bad path must fail the
# check; it is not an unavailable-capability skip.
set(missing "${work}/missing-editor${CMAKE_EXECUTABLE_SUFFIX}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PF_GUI_EXECUTABLE=${missing}"
        "${STARTUP}" --check graphicsInitializationFailure
    WORKING_DIRECTORY "${work}" RESULT_VARIABLE result
    OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 10)
if(NOT result EQUAL 1 OR NOT err STREQUAL ""
        OR NOT out MATCHES "^FAIL startup graphicsInitializationFailure: Missing required GUI executable: .+\nSUMMARY startup pass=0 fail=1 skip=0\n$")
    message(FATAL_ERROR "Missing required child was not a failure: ${result}\n${out}\n${err}")
endif()

file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "Startup smoke wrote working-directory files: ${artifacts}")
endif()
file(REMOVE_RECURSE "${work}")
