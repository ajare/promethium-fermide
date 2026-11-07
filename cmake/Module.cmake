# Included first by every promethium-fermide module CMakeLists.txt.
#
# A module is either added by the root project or configured directly with
# `cmake -S <module>`. In the second case nothing has defined the shared
# policy or dependency layer yet, so this brings both up before the module
# declares its own targets. In the first case they already exist and each
# include returns immediately.

get_filename_component(PF_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(PF_REPO_ROOT "${PF_REPO_ROOT}" CACHE PATH "promethium-fermide repository root" FORCE)

if(NOT DEFINED PF_OUTPUT_DIR)
    set(PF_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/x64/$<CONFIG>")
endif()

include("${PF_REPO_ROOT}/cmake/Policies.cmake")
include("${PF_REPO_ROOT}/cmake/Dependencies.cmake")

# The two modules whose directory name differs from the target they build.
pf_register_module_dir(promethium-fermide-core core)
pf_register_module_dir(Launcher launcher)

# Header roots for the headless tier, which exercises every module's public
# API. Linking the owning module is preferred; this exists so a check can
# include across modules the way it did against the old flat include/ tree.
# Header file names and their sub-paths are unchanged by the module split, so
# #include "core/World.h" and #include "Render.h" both still resolve.
set(PF_INCLUDE_DIR
    "${PF_REPO_ROOT}/core/include"
    "${PF_REPO_ROOT}/pf-game-runtime/include"
    "${PF_REPO_ROOT}/pf-game-authoring/include"
    "${PF_REPO_ROOT}/pf-render/include"
    "${PF_REPO_ROOT}/pf-agent-editing/include"
    "${PF_REPO_ROOT}/pf-agent-resources/include"
    "${PF_REPO_ROOT}/pf-imgui-cpu/include"
    "${PF_REPO_ROOT}/pf-imgui-cpu/include/imgui"
    "${PF_REPO_ROOT}/pf-metrics/include"
)
