#include <stdexcept>
#include <format>
#include "ApplicationDLL.h"

using namespace std;

#if APP_PLATFORM != APP_PLATFORM_WINDOWS
#define GetProcAddress(handle, name) dlsym((handle), (name))
#define FreeLibrary(handle) dlclose((handle))
#endif

string ApplicationDLL::msGetNameFunction = "dllGetName";
string ApplicationDLL::msGetNextStateFactoryFunctionName = "dllGetNextStateFactory";
string ApplicationDLL::msGetNextResourceFactoryFunctionName = "dllGetNextResourceFactory";
string ApplicationDLL::msOnEntryFunctionName = "dllOnEntry";
string ApplicationDLL::msOnExitFunctionName = "dllOnExit";
string ApplicationDLL::msCpuUpdateTimingCaptureEnabledFunctionName =
    "dllCpuUpdateTimingCaptureEnabled";
string ApplicationDLL::msRecordCpuUpdateTimingsFunctionName =
    "dllRecordCpuUpdateTimings";
string ApplicationDLL::msResetFrameRateHistoryFunctionName =
    "dllResetFrameRateHistory";
string ApplicationDLL::msRecordFrameRateFunctionName =
    "dllRecordFrameRate";
string ApplicationDLL::msCaptureScreenshotIfRequestedFunctionName =
    "dllCaptureScreenshotIfRequested";
string ApplicationDLL::msSetArgumentFunctionName = "dllSetArgument";
string ApplicationDLL::msSetInputOptionsFunctionName = "dllSetInputOptions";
string ApplicationDLL::msResetAudioSimulationOptionsFunctionName =
    "dllResetAudioSimulationOptions";
string ApplicationDLL::msAddAudioQualityPresetFunctionName =
    "dllAddAudioQualityPreset";
string ApplicationDLL::msSetAudioSimulationOptionsFunctionName =
    "dllSetAudioSimulationOptions";
string ApplicationDLL::msSetWorldDataGenerationOptionsFunctionName =
    "dllSetWorldDataGenerationOptions";
string ApplicationDLL::msSetVideoOptionsFunctionName = "dllSetVideoOptions";

ApplicationDLL::ApplicationDLL()
    : mGetProcIDDLL(0),
      mGetNameFunction(0),
      mGetNextStateFactoryFunction(0),
      mSetArgumentFunction(0),
      mSetInputOptionsFunction(0),
      mResetAudioSimulationOptionsFunction(0),
      mAddAudioQualityPresetFunction(0),
      mSetAudioSimulationOptionsFunction(0),
      mSetWorldDataGenerationOptionsFunction(0),
      mSetVideoOptionsFunction(0),
      mOnEntryFunction(0),
      mOnExitFunction(0),
      mCpuUpdateTimingCaptureEnabledFunction(0),
      mRecordCpuUpdateTimingsFunction(0),
      mResetFrameRateHistoryFunction(0),
      mRecordFrameRateFunction(0),
      mCaptureScreenshotIfRequestedFunction(0),
      mEntryStarted(false) {
}

ApplicationDLL::~ApplicationDLL() {
  unload();
}

string const& ApplicationDLL::getFilepath() const {
  return mFilepath;
}

void ApplicationDLL::registerRequiredFunctions() {
  mGetNameFunction = (DllGetNameFunction)GetProcAddress(mGetProcIDDLL, msGetNameFunction.c_str());

  if (!mGetNameFunction) {
    string errMsg = "Could not find DLL function '" + msGetNameFunction + "' in '" + mFilepath + "'.";
    throw runtime_error(errMsg.c_str());
  }

  mGetNextStateFactoryFunction = (DllGetNextStateFactoryFunction)GetProcAddress(mGetProcIDDLL, msGetNextStateFactoryFunctionName.c_str());

  if (!mGetNextStateFactoryFunction) {
    string errMsg = "Could not find DLL function '" + msGetNextStateFactoryFunctionName + "' in '" + mFilepath + "'.";
    throw runtime_error(errMsg.c_str());
  }

  mSetArgumentFunction = (DllSetArgumentFunction)GetProcAddress(mGetProcIDDLL, msSetArgumentFunctionName.c_str());

  if (!mSetArgumentFunction) {
    string errMsg = "Could not find DLL function '" + msSetArgumentFunctionName + "' in '" + mFilepath + "'.";
    throw runtime_error(errMsg.c_str());
  }

  mSetInputOptionsFunction = (DllSetInputOptionsFunction)GetProcAddress(mGetProcIDDLL, msSetInputOptionsFunctionName.c_str());

  if (!mSetInputOptionsFunction) {
    string errMsg = "Could not find DLL function '" + msSetInputOptionsFunctionName + "' in '" + mFilepath + "'.";
    throw runtime_error(errMsg.c_str());
  }

  mResetAudioSimulationOptionsFunction =
      (DllResetAudioSimulationOptionsFunction)GetProcAddress(
          mGetProcIDDLL, msResetAudioSimulationOptionsFunctionName.c_str());
  mAddAudioQualityPresetFunction =
      (DllAddAudioQualityPresetFunction)GetProcAddress(
          mGetProcIDDLL, msAddAudioQualityPresetFunctionName.c_str());
  mSetAudioSimulationOptionsFunction =
      (DllSetAudioSimulationOptionsFunction)GetProcAddress(
          mGetProcIDDLL, msSetAudioSimulationOptionsFunctionName.c_str());
  if (!mResetAudioSimulationOptionsFunction ||
      !mAddAudioQualityPresetFunction ||
      !mSetAudioSimulationOptionsFunction) {
    throw runtime_error(
        "Application is missing required audio simulation configuration exports");
  }

  mSetWorldDataGenerationOptionsFunction =
      (DllSetWorldDataGenerationOptionsFunction)GetProcAddress(
          mGetProcIDDLL,
          msSetWorldDataGenerationOptionsFunctionName.c_str());

  if (!mSetWorldDataGenerationOptionsFunction) {
    string errMsg = "Could not find DLL function '" +
                    msSetWorldDataGenerationOptionsFunctionName + "' in '" +
                    mFilepath + "'.";
    throw runtime_error(errMsg.c_str());
  }

  mSetVideoOptionsFunction = (DllSetVideoOptionsFunction)GetProcAddress(mGetProcIDDLL, msSetVideoOptionsFunctionName.c_str());

  if (!mSetVideoOptionsFunction) {
    string errMsg = "Could not find DLL function '" + msSetVideoOptionsFunctionName + "' in '" + mFilepath + "'.";
    throw runtime_error(errMsg.c_str());
  }
}

void ApplicationDLL::registerOptionalFunctions() {
  mOnEntryFunction = (DllOnEntryFunction)GetProcAddress(mGetProcIDDLL, msOnEntryFunctionName.c_str());
  mOnExitFunction = (DllOnExitFunction)GetProcAddress(mGetProcIDDLL, msOnExitFunctionName.c_str());
  mCpuUpdateTimingCaptureEnabledFunction =
      (DllCpuUpdateTimingCaptureEnabledFunction)GetProcAddress(
          mGetProcIDDLL,
          msCpuUpdateTimingCaptureEnabledFunctionName.c_str());
  mRecordCpuUpdateTimingsFunction =
      (DllRecordCpuUpdateTimingsFunction)GetProcAddress(
          mGetProcIDDLL, msRecordCpuUpdateTimingsFunctionName.c_str());
  mResetFrameRateHistoryFunction =
      (DllResetFrameRateHistoryFunction)GetProcAddress(
          mGetProcIDDLL, msResetFrameRateHistoryFunctionName.c_str());
  mRecordFrameRateFunction =
      (DllRecordFrameRateFunction)GetProcAddress(
          mGetProcIDDLL, msRecordFrameRateFunctionName.c_str());
  mCaptureScreenshotIfRequestedFunction =
      (DllCaptureScreenshotIfRequestedFunction)GetProcAddress(
          mGetProcIDDLL,
          msCaptureScreenshotIfRequestedFunctionName.c_str());
}

void ApplicationDLL::load(
    ProgramOptions const& options, wp::Logger* logger,
    wp::application::resourcesystem::ResourceManager* resourceMgr,
    wp::application::AudioSystem* audioSystem) {
  mFilepath = options.dll;

#if APP_PLATFORM == APP_PLATFORM_WINDOWS
  mGetProcIDDLL = LoadLibrary(wstring(mFilepath.begin(), mFilepath.end()).c_str());
  if (!mGetProcIDDLL) {
    auto err = GetLastError();
    throw runtime_error(std::format("Could not load '{}'. Error code: {}", mFilepath, err));
  }
#else
  mGetProcIDDLL = dlopen(mFilepath.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!mGetProcIDDLL) {
    throw runtime_error(std::format("Could not load '{}': {}", mFilepath, dlerror()));
  }
#endif

  registerRequiredFunctions();
  registerOptionalFunctions();

  // Pass in arguments
  for (auto const& argument : options.arguments) {
    if (mSetArgumentFunction(argument.first.c_str(), argument.second.c_str()) != 0) {
      string errMsg = format("Application could not parse config argument: {}={}", argument.first, argument.second);
      throw runtime_error(errMsg.c_str());
    }
  }

  // Pass in input options, before entry so the application can build its
  // input-driven objects with them
  if (mSetInputOptionsFunction(options.input.mouseSensitivity) != 0) {
    string errMsg = format("Application rejected input options: MouseSensitivity={}", options.input.mouseSensitivity);
    throw runtime_error(errMsg.c_str());
  }

  // Transfer the data-driven preset catalog before entry. Calls use only
  // scalar values and strings so no STL layout crosses the DLL boundary.
  mResetAudioSimulationOptionsFunction();
  for (auto const& preset : options.audioSimulation.presets) {
    if (mAddAudioQualityPresetFunction(
            preset.name.c_str(), preset.rayCount, preset.bounceCount,
            preset.impulseResponseDuration, preset.ambisonicOrder,
            preset.reflectionSourceCap, preset.simulationUpdateRate) != 0) {
      throw runtime_error("Application rejected audio quality preset '" +
                          preset.name + "'");
    }
  }
  auto const& audioFeatures = options.audioSimulation.features;
  if (mSetAudioSimulationOptionsFunction(
          options.audioSimulation.qualityPreset.c_str(),
          options.audioSimulation.qualityMayBeModifiedLive ? 1 : 0,
          audioFeatures.occlusion ? 1 : 0,
          audioFeatures.transmission ? 1 : 0,
          audioFeatures.reflections ? 1 : 0,
          audioFeatures.airAbsorption ? 1 : 0) != 0) {
    throw runtime_error("Application rejected audio simulation options");
  }

  // Seed application-run Generation options before entry, and therefore
  // before any map's mandatory bootstrap Generation.
  if (mSetWorldDataGenerationOptionsFunction(
          bw::app::worldDataGenerationModeCode(
              options.worldDataGeneration.mode),
          options.worldDataGeneration.startInterval,
          options.worldDataGeneration.alwaysUpdateVertices ? 1 : 0,
          options.worldDataGeneration.allowCommitIfVisible ? 1 : 0) != 0) {
    string errMsg = format(
        "Application rejected World data Generation options: Mode={}, StartInterval={}, AlwaysUpdateVertices={}, AllowCommitIfVisible={}",
        bw::app::worldDataGenerationModeName(
            options.worldDataGeneration.mode),
        options.worldDataGeneration.startInterval,
        options.worldDataGeneration.alwaysUpdateVertices,
        options.worldDataGeneration.allowCommitIfVisible);
    throw runtime_error(errMsg.c_str());
  }

  // Pass video options before entry so the model starts with the configured
  // process-wide scale and anti-aliasing setting.
  auto renderScaleCode = bw::app::renderScaleCode(options.video.renderScale);
  auto antiAliasingCode =
      bw::app::antiAliasingCode(options.video.antiAliasing);
  auto ambientOcclusionCode =
      bw::app::ambientOcclusionCode(options.video.ambientOcclusion);
  auto renderTextureFilterCode =
      bw::app::renderTextureFilterCode(options.video.renderTextureFilter);
  auto horizontalMaterialsCode =
      bw::app::horizontalMaterialsCode(options.video.horizontalMaterials);
  auto waterReflectionTechniqueCode =
      bw::app::waterReflectionTechniqueCode(
          options.video.waterReflections.technique);
  auto planarReflectionResolutionCode =
      bw::app::planarReflectionResolutionCode(
          options.video.waterReflections.planarResolution);
  auto const& playerTorch = options.video.playerTorch;
  auto const& shadows = options.video.shadows;
  if (mSetVideoOptionsFunction(
          renderScaleCode, antiAliasingCode, ambientOcclusionCode,
          renderTextureFilterCode, horizontalMaterialsCode,
          waterReflectionTechniqueCode, planarReflectionResolutionCode,
          playerTorch.attenuationRadius, playerTorch.attenuationFalloff,
          shadows.enabled ? 1 : 0,
          static_cast<uint64_t>(shadows.faceResolution), shadows.range,
          shadows.nearPlane, shadows.constantBias, shadows.normalBias,
          bw::app::shadowFilterCode(shadows.filter), shadows.filterRadius,
          shadows.fadeStart) != 0) {
    string errMsg = format(
        "Application rejected video options: RenderScale={}, AA={}, AmbientOcclusion={}, RenderTextureFilter={}, HorizontalMaterials={}, WaterReflections={}/{}, PlayerTorch={}/{}, Shadows={}/{}/{}/{}/{}/{}/{}/{}/{}",
        renderScaleCode, antiAliasingCode, ambientOcclusionCode,
        renderTextureFilterCode, horizontalMaterialsCode,
        waterReflectionTechniqueCode, planarReflectionResolutionCode,
        playerTorch.attenuationRadius, playerTorch.attenuationFalloff,
        shadows.enabled,
        shadows.faceResolution, shadows.range, shadows.nearPlane,
        shadows.constantBias, shadows.normalBias,
        bw::app::shadowFilterCode(shadows.filter), shadows.filterRadius,
        shadows.fadeStart);
    throw runtime_error(errMsg.c_str());
  }

  // Call entry function
  if (mOnEntryFunction) {
    // dllOnEntry can itself fail after constructing some DLL-owned objects.
    // Mark it started first so dllOnExit gets a chance to unwind those objects.
    mEntryStarted = true;
    mOnEntryFunction(logger, resourceMgr, audioSystem);
  }
}

void ApplicationDLL::unload() {
  // Call exit function
  if (mEntryStarted && mOnExitFunction) {
    mOnExitFunction();
    mEntryStarted = false;
  }

  if (mGetProcIDDLL != 0) {
    FreeLibrary(mGetProcIDDLL);
    mGetProcIDDLL = 0;
  }
}

string ApplicationDLL::getApplicationName() const {
  return string(mGetNameFunction());
}

bool ApplicationDLL::cpuUpdateTimingCaptureEnabled() const {
  return mCpuUpdateTimingCaptureEnabledFunction &&
         mRecordCpuUpdateTimingsFunction &&
         mCpuUpdateTimingCaptureEnabledFunction() != 0;
}

void ApplicationDLL::recordCpuUpdateTimings(
    uint64_t gameNs, uint64_t audioNs) const {
  if (mRecordCpuUpdateTimingsFunction) {
    mRecordCpuUpdateTimingsFunction(gameNs, audioNs);
  }
}

void ApplicationDLL::resetFrameRateHistory() const {
  if (mResetFrameRateHistoryFunction) {
    mResetFrameRateHistoryFunction();
  }
}

void ApplicationDLL::recordFrameRate(
    double timestampSeconds, double framesPerSecond) const {
  if (mRecordFrameRateFunction) {
    mRecordFrameRateFunction(timestampSeconds, framesPerSecond);
  }
}

void ApplicationDLL::captureScreenshotIfRequested(
    mpp::RenderSystem* renderSystem) const {
  if (mCaptureScreenshotIfRequestedFunction) {
    mCaptureScreenshotIfRequestedFunction(renderSystem);
  }
}

void ApplicationDLL::registerStateFactories(StateManager* stateMgr) {
  auto stateFactory = mGetNextStateFactoryFunction();
  while (stateFactory) {
    stateMgr->registerStateFactory(stateFactory);
    stateFactory = mGetNextStateFactoryFunction();
  }
}
