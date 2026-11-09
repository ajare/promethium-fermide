# Asserts that a module is ImGui-free (#1003, spec #995).
#
# One check, both conditions, run at the module boundary:
#
#   1. the module contains no ImGui include;
#   2. the module's link line does not carry the ImGui CPU library.
#
# This is the project's link-command assertion idiom applied to the shared
# renderer: the same shape as the compile contracts whose link line is the
# reviewable half of the contract, so the ImGui-free rule is enforced rather
# than trusted. A violation is a FATAL_ERROR, which fails the build.
#
# Reads, all required:
#   PF_IMGUI_FREE_TARGET          target name, for the failure message
#   PF_IMGUI_FREE_MODULE_DIR      module source directory to scan
#   PF_IMGUI_FREE_LIBRARY         library the module must not link
#   PF_IMGUI_FREE_LINK_LIBRARIES  the module's link line, |-separated
#
# The link line arrives as a snapshot rather than being queried here, because
# a `-P` script has no target model to ask. Any change to the link line
# reconfigures, so the snapshot cannot go stale. The source scan runs live,
# over the module's own headers and sources, at the moment the check runs.

foreach(PF_IMGUI_FREE_REQUIRED PF_IMGUI_FREE_TARGET PF_IMGUI_FREE_MODULE_DIR
        PF_IMGUI_FREE_LIBRARY PF_IMGUI_FREE_LINK_LIBRARIES)
    if(NOT DEFINED ${PF_IMGUI_FREE_REQUIRED})
        message(FATAL_ERROR
            "AssertImGuiFree.cmake requires ${PF_IMGUI_FREE_REQUIRED} to be set.")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${PF_IMGUI_FREE_MODULE_DIR}")
    message(FATAL_ERROR
        "AssertImGuiFree.cmake found no module directory at "
        "'${PF_IMGUI_FREE_MODULE_DIR}'.")
endif()

# --- Condition 1: no ImGui include ---------------------------------------
# Only preprocessor include lines are examined. Prose about ImGui - and
# there is a good deal of it, since the rule is worth stating - is not a
# violation; an #include of anything whose path names ImGui is.
file(GLOB_RECURSE PF_IMGUI_FREE_SOURCES
    "${PF_IMGUI_FREE_MODULE_DIR}/*.h"
    "${PF_IMGUI_FREE_MODULE_DIR}/*.hpp"
    "${PF_IMGUI_FREE_MODULE_DIR}/*.inl"
    "${PF_IMGUI_FREE_MODULE_DIR}/*.c"
    "${PF_IMGUI_FREE_MODULE_DIR}/*.cc"
    "${PF_IMGUI_FREE_MODULE_DIR}/*.cpp")

set(PF_IMGUI_FREE_INCLUDES "")
foreach(PF_IMGUI_FREE_FILE IN LISTS PF_IMGUI_FREE_SOURCES)
    file(STRINGS "${PF_IMGUI_FREE_FILE}" PF_IMGUI_FREE_LINES
        REGEX "^[ \t]*#[ \t]*(include|import)")
    foreach(PF_IMGUI_FREE_LINE IN LISTS PF_IMGUI_FREE_LINES)
        string(REGEX REPLACE
            "^[ \t]*#[ \t]*(include|import)[ \t]*[<\"]([^>\"]*)[>\"].*"
            "\\2" PF_IMGUI_FREE_PATH "${PF_IMGUI_FREE_LINE}")
        string(TOLOWER "${PF_IMGUI_FREE_PATH}" PF_IMGUI_FREE_PATH_LOWER)
        if(PF_IMGUI_FREE_PATH_LOWER MATCHES "imgui")
            file(RELATIVE_PATH PF_IMGUI_FREE_RELATIVE
                "${PF_IMGUI_FREE_MODULE_DIR}" "${PF_IMGUI_FREE_FILE}")
            list(APPEND PF_IMGUI_FREE_INCLUDES
                "${PF_IMGUI_FREE_RELATIVE}: ${PF_IMGUI_FREE_LINE}")
        endif()
    endforeach()
endforeach()

# --- Condition 2: the ImGui CPU library is not linked --------------------
string(REPLACE "|" ";" PF_IMGUI_FREE_LINK "${PF_IMGUI_FREE_LINK_LIBRARIES}")
set(PF_IMGUI_FREE_LINKED "")
foreach(PF_IMGUI_FREE_ENTRY IN LISTS PF_IMGUI_FREE_LINK)
    string(TOLOWER "${PF_IMGUI_FREE_ENTRY}" PF_IMGUI_FREE_ENTRY_LOWER)
    # Anything ImGui-named is refused, not only the exact library passed in:
    # a renamed or additional ImGui archive would be the same violation.
    if(PF_IMGUI_FREE_ENTRY_LOWER MATCHES "imgui")
        list(APPEND PF_IMGUI_FREE_LINKED "${PF_IMGUI_FREE_ENTRY}")
    endif()
endforeach()

if(PF_IMGUI_FREE_INCLUDES OR PF_IMGUI_FREE_LINKED)
    set(PF_IMGUI_FREE_REPORT
        "${PF_IMGUI_FREE_TARGET} must be ImGui-free (#1003, spec #995, ADR 0010).")
    if(PF_IMGUI_FREE_INCLUDES)
        list(JOIN PF_IMGUI_FREE_INCLUDES "\n    " PF_IMGUI_FREE_INCLUDE_TEXT)
        string(APPEND PF_IMGUI_FREE_REPORT
            "\n  It includes an ImGui header:\n    ${PF_IMGUI_FREE_INCLUDE_TEXT}")
    endif()
    if(PF_IMGUI_FREE_LINKED)
        list(JOIN PF_IMGUI_FREE_LINKED "\n    " PF_IMGUI_FREE_LINK_TEXT)
        string(APPEND PF_IMGUI_FREE_REPORT
            "\n  Its link line carries ${PF_IMGUI_FREE_LIBRARY} or another "
            "ImGui library:\n    ${PF_IMGUI_FREE_LINK_TEXT}")
    endif()
    string(APPEND PF_IMGUI_FREE_REPORT
        "\n  The shared renderer carries no ImGui code so that a consumer never "
        "needs an ImGui context, an ImGui header version or a linked ImGui "
        "runtime to render a World. ImGui belongs to the editor's chrome, "
        "compositing the offscreen target the renderer produces. Supply the "
        "capability through an injected seam instead - see the font metrics "
        "interface in pf-render/include/FontMetrics.h - or move the ImGui "
        "code up into the application that owns it.")
    message(FATAL_ERROR "${PF_IMGUI_FREE_REPORT}")
endif()

message(STATUS "${PF_IMGUI_FREE_TARGET} is ImGui-free: no ImGui include, "
    "no ImGui library on its link line.")
