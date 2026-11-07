# pf-metrics
#
# Prometheus text exposition and its HTTP transport. Depends only on core,
# so a headless run can publish metrics without any GUI module present.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pf-metrics-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

pf_require_module(promethium-fermide-core)
pf_require_httplib()

add_library(pf-metrics STATIC
    src/PrometheusTextFormatter.cpp
    src/MetricsHttpServer.cpp
)
# Headers keep their `metrics/` prefix, so consumers write
# #include "metrics/MetricsHttpServer.h".
target_include_directories(pf-metrics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pf-metrics
    PUBLIC promethium-fermide-core
    PRIVATE httplib::httplib)
pf_apply_project_library_policy(pf-metrics)
set_target_properties(pf-metrics PROPERTIES FOLDER "Core")
