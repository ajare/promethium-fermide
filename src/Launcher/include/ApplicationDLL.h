#pragma once

#include "Platform.h"

#if APP_PLATFORM == APP_PLATFORM_WINDOWS
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

#include "willpower/application/StateFactory.h"
#include "willpower/application/resourcesystem/ResourceManager.h"
#include "willpower/application/resourcesystem/ResourceFactory.h"

#include "willpower/common/Logger.h"

#include "ProgramOptions.h"
#include "StateManager.h"

class ApplicationDLL {
  typedef char const* (*DllGetNameFunction)();

  typedef wp::application::StateFactory* (*DllGetNextStateFactoryFunction)();

  typedef void (*DllOnEntryFunction)(
      wp::Logger*, wp::application::resourcesystem::ResourceManager*,
      wp::application::AudioSystem*);

  typedef void (*DllOnExitFunction)();

  typedef int (*DllSetArgumentFunction)(char const*, char const*);

  // Keep configuration exports to plain scalars rather than making a C++
  // struct layout part of the DLL ABI.
  typedef int (*DllSetInputOptionsFunction)(float);

  typedef void (*DllResetAudioSimulationOptionsFunction)();
  typedef int (*DllAddAudioQualityPresetFunction)(
      char const*, std::uint32_t, std::uint32_t, float, std::uint32_t,
      std::uint32_t, float);
  typedef int (*DllSetAudioSimulationOptionsFunction)(
      char const*, int, int, int, int, int);

  typedef int (*DllCpuUpdateTimingCaptureEnabledFunction)();

  typedef void (*DllRecordCpuUpdateTimingsFunction)(
      std::uint64_t, std::uint64_t);

  typedef void (*DllResetFrameRateHistoryFunction)();
  typedef void (*DllRecordFrameRateFunction)(double, double);

  typedef void (*DllCaptureScreenshotIfRequestedFunction)(mpp::RenderSystem*);

  typedef int (*DllSetWorldDataGenerationOptionsFunction)(int, float, int, int);

  // Video enums cross as stable integer codes and Torch values as ordered
  // scalars, keeping the ABI plain and independent of C++ struct layout.
  typedef int (*DllSetVideoOptionsFunction)(
      int, int, int, int, int, int, int, float, float,
      int, std::uint64_t, float, float, float, float, int, float, float);

private:
#if APP_PLATFORM == APP_PLATFORM_WINDOWS
  HINSTANCE mGetProcIDDLL;
#else
  void* mGetProcIDDLL;
#endif

  std::string mFilepath;

  std::string mName;

  // Required DLL functions
  DllGetNameFunction mGetNameFunction;

  DllGetNextStateFactoryFunction mGetNextStateFactoryFunction;

  DllSetArgumentFunction mSetArgumentFunction;

  DllSetInputOptionsFunction mSetInputOptionsFunction;

  DllResetAudioSimulationOptionsFunction mResetAudioSimulationOptionsFunction;
  DllAddAudioQualityPresetFunction mAddAudioQualityPresetFunction;
  DllSetAudioSimulationOptionsFunction mSetAudioSimulationOptionsFunction;

  DllSetWorldDataGenerationOptionsFunction
      mSetWorldDataGenerationOptionsFunction;

  DllSetVideoOptionsFunction mSetVideoOptionsFunction;

  static std::string msGetNameFunction;

  static std::string msCreateApplicationFunctionName, msDestroyApplicationFunctionName;

  static std::string msGetNextStateFactoryFunctionName;

  static std::string msGetNextResourceFactoryFunctionName;

  static std::string msSetArgumentFunctionName;

  static std::string msSetInputOptionsFunctionName;

  static std::string msResetAudioSimulationOptionsFunctionName;
  static std::string msAddAudioQualityPresetFunctionName;
  static std::string msSetAudioSimulationOptionsFunctionName;

  static std::string msSetWorldDataGenerationOptionsFunctionName;

  static std::string msSetVideoOptionsFunctionName;

  // Optional DLL functions
  DllOnEntryFunction mOnEntryFunction;

  DllOnExitFunction mOnExitFunction;

  DllCpuUpdateTimingCaptureEnabledFunction
      mCpuUpdateTimingCaptureEnabledFunction;

  DllRecordCpuUpdateTimingsFunction mRecordCpuUpdateTimingsFunction;

  DllResetFrameRateHistoryFunction mResetFrameRateHistoryFunction;
  DllRecordFrameRateFunction mRecordFrameRateFunction;

  DllCaptureScreenshotIfRequestedFunction
      mCaptureScreenshotIfRequestedFunction;

  bool mEntryStarted;

  static std::string msOnEntryFunctionName, msOnExitFunctionName;

  static std::string msCpuUpdateTimingCaptureEnabledFunctionName;

  static std::string msRecordCpuUpdateTimingsFunctionName;

  static std::string msResetFrameRateHistoryFunctionName;
  static std::string msRecordFrameRateFunctionName;

  static std::string msCaptureScreenshotIfRequestedFunctionName;

private:
  void registerRequiredFunctions();

  void registerOptionalFunctions();

public:
  ApplicationDLL();

  ~ApplicationDLL();

  std::string const& getFilepath() const;

  void load(
      ProgramOptions const& options, wp::Logger* logger,
      wp::application::resourcesystem::ResourceManager* resourceMgr,
      wp::application::AudioSystem* audioSystem);

  void unload();

  std::string getApplicationName() const;

  bool cpuUpdateTimingCaptureEnabled() const;

  void recordCpuUpdateTimings(std::uint64_t gameNs,
                              std::uint64_t audioNs) const;

  void resetFrameRateHistory() const;
  void recordFrameRate(double timestampSeconds,
                       double framesPerSecond) const;

  void captureScreenshotIfRequested(mpp::RenderSystem* renderSystem) const;

  void registerStateFactories(StateManager* stateMgr);
};
