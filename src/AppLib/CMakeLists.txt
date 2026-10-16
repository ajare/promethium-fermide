# EnTT types are part of AppLib's public entity API.
FetchContent_Declare(entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.15.0)
FetchContent_MakeAvailable(entt)

add_library(AppLib SHARED
    src/AnimationDatabase.cpp
    src/Entity.cpp
    src/EntityFacade.cpp
    src/EntityHandler.cpp
    src/EntityManager.cpp
    src/ImageSetTiledDefinitionFactory.cpp
    src/Map.cpp
    src/MapDefaultDefinitionFactory.cpp
    src/MapResourceDefinitionFactory.cpp
    src/MapTiledDefinitionFactory.cpp
    src/ModelInstance.cpp
    src/PlayObjectCreators.cpp
    src/ProtoEntity.cpp
    src/ProtoEntityDefaultDefinitionFactory.cpp
    src/ProtoEntityResourceDefinitionFactory.cpp
    src/ScreenFxManager.cpp
    src/State.cpp
    src/StateController.cpp
    src/StateLoad.cpp
    src/StateMapLoad.cpp
    src/StateMapTransition.cpp
    src/StateMapUnload.cpp
    src/StatePlay.cpp
    src/StateUnload.cpp
    src/ThreadableLoadState.cpp
    src/VisualSpriteDataProvider.cpp
    src/VisualSpriteEntityFacade.cpp
    src/VisualTriMeshEntityFacade.cpp)

# Consumers include "applib/Foo.h"; AppLib's own sources include some of those
# headers by bare name ("ModelInstance.h"), so it needs include/applib too.
target_include_directories(AppLib
    PUBLIC  include
    PRIVATE include/applib)
willpower_add_header_filter(AppLib)

target_compile_definitions(AppLib PRIVATE
    APPLIB_DLL_EXPORT
    _SILENCE_CXX17_ALLOCATOR_VOID_DEPRECATION_WARNING
    _SILENCE_CXX23_DENORM_DEPRECATION_WARNING)

target_link_libraries(AppLib PUBLIC
    EnTT::EnTT
    Willpower.Application Willpower.Common Willpower.Collide
    Willpower.Geometry WillPower.Viz
    ext::mpp ext::mpp-mesh ext::mpp-helper ext::Utils
    vendor::headers)

target_compile_features(AppLib PRIVATE cxx_std_23)
target_compile_options(AppLib PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W3>)
pf_apply_headless_verification_policy(AppLib)
willpower_enable_sdl_checks(AppLib)
set_target_properties(AppLib PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}")
set_target_properties(AppLib PROPERTIES FOLDER AppLib)

willpower_declare_resource_schema(
    RESOURCE_TYPE Map
    SCHEMA_ID
        "https://ajare.github.io/boolean-world/schemas/resource-manifest/v1/map.schema.json"
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/schemas/map.schema.json")
willpower_declare_resource_schema(
    RESOURCE_TYPE Map
    FACTORY_TYPE Tiled
    SCHEMA_ID
        "https://ajare.github.io/boolean-world/schemas/resource-manifest/v1/map-tiled.schema.json"
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/schemas/map-tiled.schema.json")
willpower_declare_resource_schema(
    RESOURCE_TYPE ProtoEntity
    SCHEMA_ID
        "https://ajare.github.io/boolean-world/schemas/resource-manifest/v1/proto-entity.schema.json"
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/schemas/proto-entity.schema.json")

if(BUILD_TESTING)
    add_executable(applib_proto_entity_schema_tests
        tests/proto_entity_schema_tests.cpp)
    target_link_libraries(applib_proto_entity_schema_tests PRIVATE AppLib)
    target_compile_features(applib_proto_entity_schema_tests PRIVATE cxx_std_23)
    pf_apply_headless_verification_policy(applib_proto_entity_schema_tests)
    willpower_deploy_runtime_dlls(applib_proto_entity_schema_tests)
    willpower_deploy_vendor_dlls(applib_proto_entity_schema_tests)
    add_test(NAME applib_proto_entity_schema COMMAND applib_proto_entity_schema_tests)

    add_executable(applib_entity_handler_tests
        tests/entity_handler_tests.cpp)
    target_link_libraries(applib_entity_handler_tests PRIVATE AppLib)
    target_compile_features(applib_entity_handler_tests PRIVATE cxx_std_23)
    pf_apply_headless_verification_policy(applib_entity_handler_tests)
    willpower_deploy_runtime_dlls(applib_entity_handler_tests)
    willpower_deploy_vendor_dlls(applib_entity_handler_tests)
    add_test(NAME applib_entity_handler COMMAND applib_entity_handler_tests)

    add_executable(applib_object_array_tests
        tests/object_array_tests.cpp)
    target_include_directories(applib_object_array_tests PRIVATE include)
    target_compile_features(applib_object_array_tests PRIVATE cxx_std_23)
    pf_apply_headless_verification_policy(applib_object_array_tests)
    add_test(NAME applib_object_array COMMAND applib_object_array_tests)
endif()
