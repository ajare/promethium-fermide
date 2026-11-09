# pf-app-bootstrap
#
# Header-only OS bootstrap for process entry points: non-interactive error
# modes and CRT reporting. The editor and the headless tier both need it and
# headless cannot link the editor, so it gets its own zero-dependency module
# instead of sitting in the World domain aggregate (#991). It links nothing,
# so requiring it costs no runtime dependency for any consumer.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pf-app-bootstrap-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

# The header is listed so IDEs and source audits see the module's whole
# surface; nothing is compiled here.
add_library(pf-app-bootstrap INTERFACE
    include/NonInteractiveProcess.h)
target_include_directories(pf-app-bootstrap INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(pf-app-bootstrap INTERFACE cxx_std_20)
set_target_properties(pf-app-bootstrap PROPERTIES FOLDER "Bootstrap")
