#pragma once

#include <willpower/common/Logger.h>
#include <willpower/application/StateFactory.h>

#if defined(_WIN32)
#if defined(PF_APPLICATION_EXPORT)
#define PF_APPLICATION_API __declspec(dllexport)
#else
#define PF_APPLICATION_API __declspec(dllimport)
#endif
#else
#define PF_APPLICATION_API __attribute__((visibility("default")))
#endif

extern "C" {
PF_APPLICATION_API char const* dllGetName();
PF_APPLICATION_API int dllSetArgument(char const* argument, char const* value);
PF_APPLICATION_API wp::application::StateFactory* dllGetNextStateFactory();
PF_APPLICATION_API void dllOnEntry(
    wp::Logger* logger,
    wp::application::resourcesystem::ResourceManager* resourceMgr,
    wp::application::AudioSystem* audioSystem);
PF_APPLICATION_API void dllOnExit();
}
