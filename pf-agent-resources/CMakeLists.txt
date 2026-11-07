# pf-agent-resources
#
# Managed Agent resource authoring and the Willpower resource registrations
# the applications share. CPU-only import seam: no dialogs, no window.
#
# ApplicationResources.h is a shared declaration surface only. Its
# implementations are deliberately split: AgentTypeResource and friends are
# implemented here, while the registration translation unit needs the
# application's Map.h and so compiles in the application tier.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pf-agent-resources-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

pf_require_module(promethium-fermide-core)
pf_require_willpower()

add_library(pf-agent-resources STATIC
    src/ApplicationAgentTypes.cpp
)
target_include_directories(pf-agent-resources PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pf-agent-resources PUBLIC promethium-fermide-core Willpower.Application)
pf_apply_project_library_policy(pf-agent-resources)
set_target_properties(pf-agent-resources PROPERTIES FOLDER "Agent")
