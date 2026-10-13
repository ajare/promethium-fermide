# Graphical shell. Application DLL selection remains a runtime concern; the
# production modules are explicit build/link dependencies, not BooleanWorld.
add_executable(Launcher WIN32
    src/ApplicationDLL.cpp
    src/DirectoryResourceLocation.cpp
    src/FrameRateTracker.cpp
    src/LauncherLifecycle.cpp
    src/Main.cpp
    src/ProgramOptions.cpp
    src/StateManager.cpp
    src/Timer.cpp
    src/Window.cpp
    src/ZipResourceLocation.cpp
    src/sdl/ImGuiSDL.cpp
    src/sdl/TimerSDL.cpp
    src/sdl/WindowSDL.cpp
    src/imgui/imgui.cpp
    src/imgui/imgui_demo.cpp
    src/imgui/imgui_draw.cpp
    src/imgui/imgui_tables.cpp
    src/imgui/imgui_widgets.cpp
    src/imgui/implot.cpp
    src/imgui/implot_demo.cpp
    src/imgui/implot_items.cpp)

target_include_directories(Launcher PRIVATE include include/imgui)
willpower_add_header_filter(Launcher)
target_link_libraries(Launcher PRIVATE
    AppLib
    Willpower.Common Willpower.Application Willpower.Geometry
    Willpower.Collide Willpower.Wayfinder WillPower.Viz
    promethium-fermide-core pf-render pf-metrics
    pf-agent-editing pf-agent-resources
    ext::mpp ext::Utils ext::sdl OpenGL::GL)
pf_apply_headless_verification_policy(Launcher)
willpower_enable_sdl_checks(Launcher)
set_target_properties(Launcher PROPERTIES
    FOLDER Launcher
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:Launcher>")
willpower_deploy_runtime_dlls(Launcher)
willpower_deploy_vendor_dlls(Launcher)

if(BUILD_TESTING)
    add_executable(launcher_lifecycle_tests
        tests/launcher_lifecycle_tests.cpp
        src/FrameRateTracker.cpp
        src/LauncherLifecycle.cpp)
    target_include_directories(launcher_lifecycle_tests PRIVATE include)
    pf_apply_headless_verification_policy(launcher_lifecycle_tests)
    add_test(NAME launcher_lifecycle COMMAND launcher_lifecycle_tests)

    add_executable(launcher_program_options_tests
        tests/program_options_tests.cpp src/ProgramOptions.cpp)
    target_include_directories(launcher_program_options_tests PRIVATE include)
    target_link_libraries(launcher_program_options_tests PRIVATE
        Willpower.Common Willpower.Application ext::Utils)
    target_compile_definitions(launcher_program_options_tests PRIVATE
        BW_LAUNCHER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    pf_apply_headless_verification_policy(launcher_program_options_tests)
    willpower_deploy_runtime_dlls(launcher_program_options_tests)
    willpower_deploy_vendor_dlls(launcher_program_options_tests)
    add_test(NAME launcher_program_options COMMAND launcher_program_options_tests
        "$<TARGET_FILE_DIR:Launcher>/Game.yaml" "${PROJECT_SOURCE_DIR}/resources")

    add_executable(launcher_window_input_tests
        tests/window_input_tests.cpp
        src/StateManager.cpp src/Window.cpp
        src/sdl/ImGuiSDL.cpp src/sdl/WindowSDL.cpp
        src/imgui/imgui.cpp src/imgui/imgui_draw.cpp
        src/imgui/imgui_tables.cpp src/imgui/imgui_widgets.cpp)
    target_include_directories(launcher_window_input_tests PRIVATE include include/imgui)
    target_link_libraries(launcher_window_input_tests PRIVATE
        Willpower.Common Willpower.Application ext::mpp ext::Utils ext::sdl)
    pf_apply_headless_verification_policy(launcher_window_input_tests)
    willpower_enable_sdl_checks(launcher_window_input_tests)
    willpower_deploy_runtime_dlls(launcher_window_input_tests)
    willpower_deploy_vendor_dlls(launcher_window_input_tests)
    add_test(NAME launcher_window_input COMMAND launcher_window_input_tests)

    if(WIN32)
        add_executable(launcher_zip_resource_location_tests
            tests/zip_resource_location_tests.cpp src/ZipResourceLocation.cpp)
        target_include_directories(launcher_zip_resource_location_tests PRIVATE include)
        target_link_libraries(launcher_zip_resource_location_tests PRIVATE
            Willpower.Common Willpower.Application ext::Utils)
        pf_apply_headless_verification_policy(launcher_zip_resource_location_tests)
        willpower_deploy_runtime_dlls(launcher_zip_resource_location_tests)
        willpower_deploy_vendor_dlls(launcher_zip_resource_location_tests)
        add_test(NAME launcher_zip_resource_location COMMAND launcher_zip_resource_location_tests)
    endif()
endif()
