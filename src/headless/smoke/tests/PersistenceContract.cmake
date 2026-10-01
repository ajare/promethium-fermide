# Exercise the public CLI outside both source and build trees.
if(WIN32)
    set(temp "$ENV{TEMP}")
else()
    set(temp "/tmp")
endif()
string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef suffix)
set(work "${temp}/pf-persistence-contract-${suffix}")
file(MAKE_DIRECTORY "${work}")

function(invoke status expected)
    execute_process(COMMAND "${PERSISTENCE}" ${ARGN}
        WORKING_DIRECTORY "${work}" RESULT_VARIABLE result
        OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 15)
    if(NOT "${result}" STREQUAL "${status}")
        message(FATAL_ERROR "${ARGN}: expected ${status}, got ${result}\n${out}\n${err}")
    endif()
    if(status EQUAL 2)
        if(NOT out STREQUAL "" OR NOT err MATCHES "^ERROR persistence:")
            message(FATAL_ERROR "Unexpected misuse output: ${out} / ${err}")
        endif()
    elseif(NOT err STREQUAL "" OR NOT out MATCHES "${expected}")
        message(FATAL_ERROR "Unexpected result: ${out} / ${err}")
    endif()
endfunction()

set(names yaml-primitives binary-contract yaml-file transactional-bytes
    world-document-formats yaml-errors checked-in-world)
string(JOIN "\n" listing ${names})
invoke(0 "^${listing}\n$" --list)
invoke(0 "SUMMARY persistence pass=7 fail=0 skip=0\n$")
foreach(name IN LISTS names)
    invoke(0 "^PASS persistence ${name}\nSUMMARY persistence pass=1 fail=0 skip=0\n$" --check "${name}")
endforeach()
foreach(arguments IN ITEMS "--bogus" "--check" "--check;absent" "--list;extra" "--check;yaml-file;extra")
    invoke(2 "" ${arguments})
endforeach()
file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "Persistence wrote working-directory files: ${artifacts}")
endif()

# A private CTest project launches concurrent independent processes portably.
# They all share a cwd and OS temp parent; only their Context roots differ.
set(project "${work}/concurrent")
file(MAKE_DIRECTORY "${project}")
file(WRITE "${project}/CTestTestfile.cmake" "")
foreach(index RANGE 1 8)
    file(APPEND "${project}/CTestTestfile.cmake"
        "add_test(persistence-${index} \"${PERSISTENCE}\")\n"
        "set_tests_properties(persistence-${index} PROPERTIES TIMEOUT 15 WORKING_DIRECTORY \"${work}\" PASS_REGULAR_EXPRESSION \"SUMMARY persistence pass=7 fail=0 skip=0\" FAIL_REGULAR_EXPRESSION \"FAIL persistence\")\n")
endforeach()
find_program(ctest NAMES ctest REQUIRED)
execute_process(COMMAND "${ctest}" --test-dir "${project}" -j 8 --output-on-failure
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Concurrent Persistence invocations failed: ${result}\n${out}\n${err}")
endif()
file(REMOVE_RECURSE "${project}")
file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "Concurrent Persistence wrote working-directory files: ${artifacts}")
endif()
file(REMOVE_RECURSE "${work}")
