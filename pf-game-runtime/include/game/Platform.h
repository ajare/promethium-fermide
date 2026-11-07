#pragma once

// Platform settings for pf-game-runtime, following the same export-macro
// convention as `core/Platform.h`, `APPLIB_API` and the `WP_*_API` macros:
//
//   * building pf-game-runtime defines PF_GAME_RUNTIME_DLL_EXPORT privately,
//     so PF_GAME_RUNTIME_API resolves to an export;
//   * every consumer compiles without that definition, so PF_GAME_RUNTIME_API
//     resolves to an import.
//
// pf-game-runtime is SHARED (#964, part of #960) so exactly one copy of the
// runtime exists in a process, and the World it owns is created and destroyed
// on one side of a real boundary rather than being embedded per consumer.
//
// `template` and `inline` members stay header-only and are not annotated: they
// are instantiated in the including translation unit and cannot be exported.

#define PF_GAME_RUNTIME_PLATFORM_WINDOWS 1
#define PF_GAME_RUNTIME_PLATFORM_LINUX 2
#define PF_GAME_RUNTIME_PLATFORM_APPLE 3

#define PF_GAME_RUNTIME_COMPILER_MSVC 1
#define PF_GAME_RUNTIME_COMPILER_GNUC 2

#if defined(_MSC_VER)
#define PF_GAME_RUNTIME_COMPILER PF_GAME_RUNTIME_COMPILER_MSVC
#define PF_GAME_RUNTIME_COMP_VER _MSC_VER
#elif defined(__GNUC__)
#define PF_GAME_RUNTIME_COMPILER PF_GAME_RUNTIME_COMPILER_GNUC
#define PF_GAME_RUNTIME_COMP_VER (((__GNUC__) * 100) + (__GNUC_MINOR__ * 10) + __GNUC_PATCHLEVEL__)
#else
#error "Unknown compiler."
#endif

#if defined(__WIN32__) || defined(_WIN32)
#define PF_GAME_RUNTIME_PLATFORM PF_GAME_RUNTIME_PLATFORM_WINDOWS
#elif defined(__APPLE_CC__)
#define PF_GAME_RUNTIME_PLATFORM PF_GAME_RUNTIME_PLATFORM_APPLE
#else
#define PF_GAME_RUNTIME_PLATFORM PF_GAME_RUNTIME_PLATFORM_LINUX
#endif

#if PF_GAME_RUNTIME_PLATFORM == PF_GAME_RUNTIME_PLATFORM_WINDOWS
#if defined(PF_GAME_RUNTIME_DLL_EXPORT)
#define PF_GAME_RUNTIME_API __declspec(dllexport)
#elif defined(PF_GAME_RUNTIME_STATIC_LIB)
#define PF_GAME_RUNTIME_API
#else
#if defined(__MINGW32__)
#define PF_GAME_RUNTIME_API
#else
#define PF_GAME_RUNTIME_API __declspec(dllimport)
#endif
#endif
#else
#if defined(PF_GAME_RUNTIME_DLL_EXPORT)
#define PF_GAME_RUNTIME_API __attribute__((visibility("default")))
#else
#define PF_GAME_RUNTIME_API
#endif
#endif

// The runtime's public surface carries STL members (std::unique_ptr,
// std::vector) across the boundary. Consumers are in-repo, same toolchain,
// same CRT, which is the accepted contract for this boundary (#960).
#if PF_GAME_RUNTIME_PLATFORM == PF_GAME_RUNTIME_PLATFORM_WINDOWS
#pragma warning(disable : 4251)
#endif
