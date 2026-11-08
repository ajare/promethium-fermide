# pf-agent-editing
#
# Production Agent and permission editing seams, shared by the GUI, the
# legacy checks, and the headless editor tiers. No platform backend and no
# dialogs: the applications supply those.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pf-agent-editing-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

pf_require_module(promethium-fermide-core)
pf_require_module(pf-imgui-cpu)
pf_require_module(pf-render)
pf_require_module(pf-game-authoring)

add_library(pf-agent-editing STATIC
    src/AccessPanelEditor.cpp
    src/AgentBehaviourAssignmentPanel.cpp
    src/AgentClipboard.cpp
    src/AgentGroupAssignmentPanel.cpp
    src/AgentGroupsPanel.cpp
    src/AgentTagAssignmentPanel.cpp
    src/BehavioursPanel.cpp
    src/BoothWindowEditor.cpp
    src/ChamberPanel.cpp
    src/DocumentEdit.cpp
    src/DoorClipboard.cpp
    src/DoorPanel.cpp
    src/EditorDocument.cpp
    src/FurniturePanel.cpp
    src/LocationPlan.cpp
    src/LocationPlanRender.cpp
    src/MarkerPanel.cpp
    src/PermissionsPanel.cpp
    src/TagsPanel.cpp
)
target_include_directories(pf-agent-editing PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pf-agent-editing PUBLIC promethium-fermide-core pf-imgui-cpu
    pf-render pf-game-authoring)
target_compile_definitions(pf-agent-editing PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING>
)
pf_apply_project_library_policy(pf-agent-editing)
set_target_properties(pf-agent-editing PROPERTIES FOLDER "Agent")
