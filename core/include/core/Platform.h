#pragma once

// Platform settings - based off OGRE3D (www.ogre3d.org), matching the
// APPLIB_API and WP_*_API export-macro convention already established in
// this repo.
//
// promethium-fermide-core is a STATIC library today. The macro is declared
// now, ahead of any linkage change, so the export surface is explicit and
// compile-verified before the risk is taken (#961, part of #960):
//
//   * building core defines PF_CORE_DLL_EXPORT privately, so PF_CORE_API
//     resolves to an export;
//   * every consumer compiles without that definition, so PF_CORE_API
//     resolves to an import.
//
// `template` and `inline` members are deliberately left header-only and are
// not annotated: they are instantiated in the including translation unit and
// cannot be exported.

#define PF_CORE_PLATFORM_WINDOWS 1
#define PF_CORE_PLATFORM_LINUX 2
#define PF_CORE_PLATFORM_APPLE 3

#define PF_CORE_COMPILER_MSVC 1
#define PF_CORE_COMPILER_GNUC 2
#define PF_CORE_COMPILER_BORL 3

// Find compiler information
#if defined(_MSC_VER)
#define PF_CORE_COMPILER PF_CORE_COMPILER_MSVC
#define PF_CORE_COMP_VER _MSC_VER
#elif defined(__GNUC__)
#define PF_CORE_COMPILER PF_CORE_COMPILER_GNUC
#define PF_CORE_COMP_VER (((__GNUC__) * 100) +    \
                         (__GNUC_MINOR__ * 10) + \
                         __GNUC_PATCHLEVEL__)
#elif defined(__BORLANDC__)
#define PF_CORE_COMPILER PF_CORE_COMPILER_BORL
#define PF_CORE_COMP_VER __BCPLUSPLUS__
#else
#pragma error "Unknown compiler."
#endif

// Set platform
#if defined(__WIN32__) || defined(_WIN32)
#define PF_CORE_PLATFORM PF_CORE_PLATFORM_WINDOWS
#elif defined(__APPLE_CC__)
#define PF_CORE_PLATFORM PF_CORE_PLATFORM_APPLE
#else
#define PF_CORE_PLATFORM PF_CORE_PLATFORM_LINUX
#endif

// DLL Export
#if PF_CORE_PLATFORM == PF_CORE_PLATFORM_WINDOWS
#if defined(PF_CORE_DLL_EXPORT)
#define PF_CORE_API __declspec(dllexport)
#elif defined(PF_CORE_STATIC_LIB)
#define PF_CORE_API
#else
#if defined(__MINGW32__)
#define PF_CORE_API
#else
#define PF_CORE_API __declspec(dllimport)
#endif
#endif
#else
#if defined(PF_CORE_DLL_EXPORT)
#define PF_CORE_API __attribute__((visibility("default")))
#else
#define PF_CORE_API
#endif
#endif

// Ok, because only occurs on non-public STL members
#if PF_CORE_PLATFORM == PF_CORE_PLATFORM_WINDOWS
#pragma warning(disable : 4251)
#endif
