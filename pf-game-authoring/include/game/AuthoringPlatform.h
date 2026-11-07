#pragma once

// Platform settings for pf-game-authoring, following the same export-macro
// convention as `core/Platform.h`, `APPLIB_API` and the runtime's
// `game/Platform.h`:
//
//   * building pf-game-authoring defines PF_GAME_AUTHORING_DLL_EXPORT
//     privately, so PF_GAME_AUTHORING_API resolves to an export;
//   * every consumer compiles without that definition, so
//     PF_GAME_AUTHORING_API resolves to an import.
//
// The header is deliberately not called `game/Platform.h`: that name already
// belongs to pf-game-runtime and both directories are include roots, so a
// second `game/Platform.h` would make which one a TU gets depend on include
// order. The authoring library's own name is carried in the file name instead.
//
// pf-game-authoring is SHARED (#967, part of #960) so exactly one copy of
// the authoring surface exists in a process, above the one copy of the
// runtime it wraps.
//
// `template` and `inline` members stay header-only and are not annotated: they
// are instantiated in the including translation unit and cannot be exported.

#define PF_GAME_AUTHORING_PLATFORM_WINDOWS 1
#define PF_GAME_AUTHORING_PLATFORM_LINUX 2
#define PF_GAME_AUTHORING_PLATFORM_APPLE 3

#define PF_GAME_AUTHORING_COMPILER_MSVC 1
#define PF_GAME_AUTHORING_COMPILER_GNUC 2

#if defined(_MSC_VER)
#define PF_GAME_AUTHORING_COMPILER PF_GAME_AUTHORING_COMPILER_MSVC
#define PF_GAME_AUTHORING_COMP_VER _MSC_VER
#elif defined(__GNUC__)
#define PF_GAME_AUTHORING_COMPILER PF_GAME_AUTHORING_COMPILER_GNUC
#define PF_GAME_AUTHORING_COMP_VER (((__GNUC__) * 100) + (__GNUC_MINOR__ * 10) + __GNUC_PATCHLEVEL__)
#else
#error "Unknown compiler."
#endif

#if defined(__WIN32__) || defined(_WIN32)
#define PF_GAME_AUTHORING_PLATFORM PF_GAME_AUTHORING_PLATFORM_WINDOWS
#elif defined(__APPLE_CC__)
#define PF_GAME_AUTHORING_PLATFORM PF_GAME_AUTHORING_PLATFORM_APPLE
#else
#define PF_GAME_AUTHORING_PLATFORM PF_GAME_AUTHORING_PLATFORM_LINUX
#endif

#if PF_GAME_AUTHORING_PLATFORM == PF_GAME_AUTHORING_PLATFORM_WINDOWS
#if defined(PF_GAME_AUTHORING_DLL_EXPORT)
#define PF_GAME_AUTHORING_API __declspec(dllexport)
#elif defined(PF_GAME_AUTHORING_STATIC_LIB)
#define PF_GAME_AUTHORING_API
#else
#if defined(__MINGW32__)
#define PF_GAME_AUTHORING_API
#else
#define PF_GAME_AUTHORING_API __declspec(dllimport)
#endif
#endif
#else
#if defined(PF_GAME_AUTHORING_DLL_EXPORT)
#define PF_GAME_AUTHORING_API __attribute__((visibility("default")))
#else
#define PF_GAME_AUTHORING_API
#endif
#endif

// The authoring surface carries STL members across the boundary as the
// families land on it (std::string in the add/rename/edit signatures, and the
// core read-model types the preflight guards report). Consumers are in-repo,
// same toolchain, same CRT, which is the accepted contract for this boundary
// (#960).
#if PF_GAME_AUTHORING_PLATFORM == PF_GAME_AUTHORING_PLATFORM_WINDOWS
#pragma warning(disable : 4251)
#endif
