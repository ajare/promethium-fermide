#include <stdexcept>
#include <algorithm>
#include <format>
#include <iostream>
#include <optional>

#include "utils/StringUtils.h"

#include "Platform.h"

#if APP_PLATFORM == APP_PLATFORM_WINDOWS
#include <windows.h>

// Ask a hybrid-graphics laptop for its discrete GPU. Both vendors' drivers
// look these symbols up in the export table of the process's own executable,
// so they belong here and not in any DLL the Launcher loads. Without them an
// OpenGL context lands on the integrated adapter: on a Radeon 610M / RTX 5070
// machine the game rendered on the 610M and was GPU-bound at roughly 72 fps
// with a 512x320 render target.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#include <willpower/common/Exceptions.h>
#include <willpower/common/Logger.h>
#include <willpower/common/Timer.h>

#include <willpower/application/ServiceLocator.h>
#include <willpower/application/AudioSystem.h>
#include <willpower/application/ApplicationSettings.h>
#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/application/resourcesystem/ResourceExceptions.h>

// The GL enums the ImGui font texture is declared with. These used to arrive
// through WindowGLFW.h; the SDL backend pulls in no GL headers of its own.
#include <GL/glew.h>

#include <mpp/MppException.h>
#include <mpp/RenderSystem.h>
#include <mpp/ResourceManager.h>
#include <mpp/ProgrammaticTextureStream.h>
#include <mpp/Logger.h>
#include <mpp/BufferRenderer.h>

// Provides the WinMain that forwards to main() below, so the Launcher stays a
// WIN32-subsystem executable without a platform-specific entry point.
#include <SDL3/SDL_main.h>

#include "imgui/imgui.h"
#include "imgui/implot.h"

#include "ProgramOptions.h"
#include "ApplicationDLL.h"
#include "FrameRateTracker.h"
#include "LauncherLifecycle.h"
#include "StateManager.h"
#include "ExitApplicationException.h"
#include "DirectoryResourceLocation.h"
#include "ZipResourceLocation.h"
#include "ImGuiDataProvider.h"

#include "sdl/WindowSDL.h"
#include "sdl/TimerSDL.h"
#include "sdl/ImGuiSDL.h"

using namespace std;
using namespace wp;

// Logging
Logger* gLogger = nullptr;
mpp::Logger* gMppLogger = nullptr;

// Platform objects
static ApplicationDLL* gDLL = nullptr;
static StateManager* gStateMgr = nullptr;
static wp::application::AudioSystem* gAudioSystem = nullptr;

static WindowSDL* gWindow = nullptr;
static TimerSDL* gTimer = nullptr;

// Application objects
static application::ApplicationSettings* gAppSettings = nullptr;
static application::resourcesystem::ResourceManager* gResourceMgr = nullptr;

bool gDisplayDebugEnabled = false;
static FrameRateTracker gGameFrameRate;

// Rendering objects
static mpp::RenderSystem* gRenderSystem = nullptr;
static mpp::ResourceManager* gRenderSystemResourceMgr = nullptr;
static shared_ptr<ImGuiDataProvider> gImGuiDataProvider;
static mpp::BufferRenderer* gImGuiRenderer = nullptr;

// Seconds since SDL started, at the performance counter's resolution - the
// replacement for glfwGetTime(). Only differences are used, so the epoch does
// not matter.
static double elapsedSeconds() {
  static uint64_t frequency = SDL_GetPerformanceFrequency();
  return (double)SDL_GetPerformanceCounter() / (double)frequency;
}

#pragma warning(push)
#pragma warning(disable : 4996)  // [DEBUG-a4f2] getenv/fopen in throwaway instrumentation

// [DEBUG-a4f2] Frame capture for rendering diagnosis. Set BW_CAPTURE_AFTER to a
// number of seconds; the first frame past that point is read back from the
// draw buffer, written to BW_CAPTURE_FILE as a 32-bit BMP, and the app exits.
// BMP rather than PNG because miniz's writer lives in another TU here, and
// 32bpp so rows need no padding. glReadPixels and BI_RGB are both bottom-up,
// so the rows already line up.
// [DEBUG-a4f2] GL state left behind by the final world draw call. Logged once,
// straight after the world render, so it reflects the state walls were drawn
// with rather than whatever ImGui leaves at end of frame.
static void logWorldRenderGlState() {
  static bool logged = false;
  if (logged || !getenv("BW_CAPTURE_AFTER")) {
    return;
  }
  logged = true;

  GLint drawFbo = -1, depthBits = -1, depthFunc = 0;
  GLboolean depthWrite = GL_FALSE;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
  glGetIntegerv(GL_DEPTH_BITS, &depthBits);
  glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);

  gLogger->info(format("[DEBUG-a4f2] after world render: drawFbo={} depthBits={} depthTest={} depthWrite={} depthFunc=0x{:X}",
                       drawFbo, depthBits, glIsEnabled(GL_DEPTH_TEST) == GL_TRUE,
                       depthWrite == GL_TRUE, (unsigned)depthFunc));
}

static void captureFrameIfRequested() {
  char const* after = getenv("BW_CAPTURE_AFTER");
  if (!after) {
    return;
  }

  if (gTimer->getTotalTime() < atof(after)) {
    return;
  }

  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(gWindow->getWindow(), &w, &h);
  if (w <= 0 || h <= 0) {
    return;
  }

  std::vector<uint8_t> pixels((size_t)w * h * 4);

  if (getenv("BW_CAPTURE_DEPTH")) {
    // Depth as greyscale, contrast-stretched over the range actually present -
    // raw depth is bunched up near 1.0 and would look uniformly white.
    std::vector<float> depth((size_t)w * h);
    glReadPixels(0, 0, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());

    float lo = 1.0f, hi = 0.0f;
    for (float d : depth) {
      if (d < lo) lo = d;
      if (d > hi) hi = d;
    }
    float span = (hi - lo) > 1e-9f ? (hi - lo) : 1.0f;

    for (size_t i = 0; i < depth.size(); ++i) {
      auto v = (uint8_t)(255.0f * (1.0f - (depth[i] - lo) / span));
      pixels[i * 4 + 0] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = v;
      pixels[i * 4 + 3] = 255;
    }

    gLogger->info(format("[DEBUG-a4f2] depth range {} .. {}", lo, hi));
  } else {
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
  }

  char const* path = getenv("BW_CAPTURE_FILE");
  FILE* f = fopen(path ? path : "capture.bmp", "wb");
  if (f) {
    uint32_t dataSize = (uint32_t)pixels.size();
    uint32_t fileSize = 14 + 40 + dataSize;
    uint32_t offset = 14 + 40;
    uint16_t zero = 0, planes = 1, bpp = 32;
    uint32_t hdr = 40, compression = 0, zero32 = 0;
    int32_t iw = w, ih = h;

    fwrite("BM", 1, 2, f);
    fwrite(&fileSize, 4, 1, f);
    fwrite(&zero, 2, 1, f);
    fwrite(&zero, 2, 1, f);
    fwrite(&offset, 4, 1, f);
    fwrite(&hdr, 4, 1, f);
    fwrite(&iw, 4, 1, f);
    fwrite(&ih, 4, 1, f);
    fwrite(&planes, 2, 1, f);
    fwrite(&bpp, 2, 1, f);
    fwrite(&compression, 4, 1, f);
    fwrite(&dataSize, 4, 1, f);
    for (int i = 0; i < 4; ++i) {
      fwrite(&zero32, 4, 1, f);
    }
    fwrite(pixels.data(), 1, pixels.size(), f);
    fclose(f);
  }

  throw ExitApplicationException(0, "[DEBUG-a4f2] frame captured");
}

#pragma warning(pop)

void initialiseImGui(float contentScale) {
  ImGuiIO& io = ImGui::GetIO();

  // Configure ImGui
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  // TODO: Set optional io.ConfigFlags values, e.g. 'io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard' to enable keyboard controls.
  // TODO: Fill optional fields of the io structure later.
  // TODO: Load TTF/OTF fonts if you don't want to use the default font.
  ImFontConfig fontCfg;
  fontCfg.SizePixels = 13.0f * contentScale;

  io.Fonts->AddFontDefault(&fontCfg);

  auto fontRes = gRenderSystemResourceMgr->getResource("__ImGui_Font__", true);
  if (!fontRes) {
    int fontWidth, fontHeight;
    unsigned char* fontData{nullptr};

    io.Fonts->GetTexDataAsRGBA32(&fontData, &fontWidth, &fontHeight);

    auto fontTextureStr = new mpp::ProgrammaticTextureStream(gRenderSystemResourceMgr);

    fontTextureStr->setTarget(mpp::TextureTarget::Texture2D);
    fontTextureStr->setData([fontData, fontWidth, fontHeight](string const&) {
      mpp::TextureData data;

      data.width = fontWidth;
      data.height = fontHeight;
      data.bitsPerPixel = 32;
      data.dataType = GL_UNSIGNED_BYTE;
      data.pixelFormat = GL_RGBA;

      size_t dataSize = (data.width * data.height * data.bitsPerPixel / 8);

      data.data = new uint8_t[dataSize];
      memcpy(data.data, fontData, dataSize);

      return data;
    });

    fontTextureStr->setFiltering(mpp::TextureParams::MinFilter::Linear, mpp::TextureParams::MagFilter::Linear);

    fontRes = gRenderSystemResourceMgr->declareResource("__ImGui_Font__", mpp::ResourceStreamPtr(fontTextureStr)).first;
    fontRes->load();
  }

  io.Fonts->SetTexID((ImTextureID)(intptr_t)fontRes->getId());

  io.DisplaySize.x = (float)gRenderSystem->getWindowWidth();
  io.DisplaySize.y = (float)gRenderSystem->getWindowHeight();

  ImGui::StyleColorsDark();
  ImGui::GetStyle().ScaleAllSizes(contentScale);
}

//
// Initialise all systems
//
ProgramOptions startup(string const& configFile, LauncherLifecycle& lifecycle) {
  using Service = LauncherLifecycle::Service;

  // Create loggers
  gLogger = new Logger();
  lifecycle.track(Service::Logger, []() {
    delete gLogger;
    gLogger = nullptr;
  });
  gLogger->open("LauncherLog.html");

  gMppLogger = new mpp::Logger();
  lifecycle.track(Service::MppLogger, []() {
    delete gMppLogger;
    gMppLogger = nullptr;
  });
  if (!gMppLogger->initialise("mpp.log", mpp::Logger::Level::Debug)) {
    throw runtime_error("Could not create MPP logger!");
  }

  // Read in program options
  ProgramOptions options = parseProgramOptions(configFile);

  logProgramOptions(options, gLogger);

  // Set up application settings
  gAppSettings = new application::ApplicationSettings();
  gAppSettings->VideoWidth = options.screenWidth;
  gAppSettings->VideoHeight = options.screenHeight;
  gAppSettings->Fullscreen = options.fullScreen;

  application::ServiceLocator::provideApplicatonSettings(gAppSettings);
  lifecycle.track(Service::ApplicationSettings, []() {
    application::ServiceLocator::provideApplicatonSettings(nullptr);
    delete gAppSettings;
    gAppSettings = nullptr;
  });

#if defined(__linux__)
  // The vendored GLEW build uses its GLX backend. SDL otherwise prefers
  // Wayland when both Wayland and XWayland are available, producing a valid
  // EGL context that GLEW cannot initialise ("No GLX display"). Respect an
  // explicit user choice, but default the Launcher to SDL's X11 backend.
  if (!SDL_getenv("SDL_VIDEODRIVER") &&
      !SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11")) {
    throw runtime_error("Could not select SDL's X11 video driver for GLX.");
  }
#endif
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    throw runtime_error("Could not initialise SDL subsystem: " +
                        string(SDL_GetError()));
  }
  lifecycle.track(Service::Platform, []() { SDL_Quit(); });
  gLogger->info("SDL video driver: " + string(SDL_GetCurrentVideoDriver()));

  // Create timer
  gTimer = new TimerSDL();
  lifecycle.track(Service::Timer, []() {
    delete gTimer;
    gTimer = nullptr;
  });

  // Create window
  gWindow = new WindowSDL("Window", options);
  lifecycle.track(Service::Window, []() {
    delete gWindow;
    gWindow = nullptr;
  });
  gWindow->create();

  // Create render system
  // mpp::enable_static_log(MPP_RESOURCE_LOGFILE, true);

  gRenderSystem = new mpp::RenderSystem(gWindow->getWidth(), gWindow->getHeight(), gMppLogger);
  lifecycle.track(Service::RenderSystem, []() {
    delete gRenderSystem;
    gRenderSystem = nullptr;
  });
  gRenderSystemResourceMgr = new mpp::ResourceManager(gRenderSystem, gMppLogger);
  lifecycle.track(Service::RenderResourceManager, []() {
    gRenderSystemResourceMgr->dumpResources("final-resources.csv");
    delete gRenderSystemResourceMgr;
    gRenderSystemResourceMgr = nullptr;
  });
  lifecycle.track(Service::RenderCoreResources, []() {
    if (gRenderSystem) {
      gRenderSystem->destroyCoreResources();
    }
  });
  gRenderSystem->createCoreResources(gRenderSystemResourceMgr);

  // Audio
  gAudioSystem = options.audioEnabled ? new wp::application::AudioSystem(options.audio) : nullptr;
  if (gAudioSystem) {
    lifecycle.track(Service::AudioSystem, []() {
      delete gAudioSystem;
      gAudioSystem = nullptr;
    });
  }

  // Resource manager
  gResourceMgr = new application::resourcesystem::ResourceManager(gRenderSystem, gRenderSystemResourceMgr, gAudioSystem, gLogger);
  lifecycle.track(Service::ResourceManager, []() {
    delete gResourceMgr;
    gResourceMgr = nullptr;
  });

  // Add resource location factories
  gResourceMgr->addResourceLocationFactory("Directory", [](string const& location, string const& definitionFile) -> application::resourcesystem::ResourceLocation* {
    return new DirectoryResourceLocation(gLogger, location, definitionFile);
  });

  gResourceMgr->addResourceLocationFactory("ZipFile", [](string const& location, string const& definitionFile) -> application::resourcesystem::ResourceLocation* {
    return new ZipResourceLocation(gLogger, location, definitionFile);
  });

  // Add resource locations
  for (auto const& rl : options.resourceLocations) {
    gResourceMgr->addResourceLocation(rl.type, rl.path, rl.definitionFile);
  }

  // ImGui. Track each context as soon as it exists so failures later in
  // initialisation still unwind only the portions that were constructed.
  auto imGuiContext = ImGui::CreateContext();
  lifecycle.track(Service::ImGuiContext, [imGuiContext]() { ImGui::DestroyContext(imGuiContext); });
  auto imPlotContext = ImPlot::CreateContext();
  lifecycle.track(Service::ImPlotContext, [imPlotContext]() { ImPlot::DestroyContext(imPlotContext); });
  initialiseImGuiForSdl(gWindow->getWindow());
  lifecycle.track(Service::ImGuiBackend, []() { shutdownImGuiForSdl(); });
  initialiseImGui(gWindow->getContentScale());

  vector<mpp::ResourcePtr> imGuiTextures;
  imGuiTextures.push_back(gRenderSystemResourceMgr->getResource("__ImGui_Font__"));

  gImGuiDataProvider = make_shared<ImGuiDataProvider>(imGuiTextures);
  lifecycle.track(Service::ImGuiDataProvider, []() { gImGuiDataProvider.reset(); });
  gImGuiRenderer = new mpp::BufferRenderer(gImGuiDataProvider);
  lifecycle.track(Service::ImGuiRenderer, []() {
    delete gImGuiRenderer;
    gImGuiRenderer = nullptr;
  });

  // Load application DLL
  gDLL = new ApplicationDLL();
  lifecycle.track(Service::ApplicationDll, []() {
    delete gDLL;
    gDLL = nullptr;
  });
  gDLL->load(options, gLogger, gResourceMgr, gAudioSystem);

  // Create state manager and get state factories
  gStateMgr = new StateManager(gResourceMgr, gAudioSystem, gRenderSystem, gRenderSystemResourceMgr);
  lifecycle.track(Service::StateManager, []() {
    if (gWindow) {
      gWindow->setStateManager(nullptr);
    }
    delete gStateMgr;
    gStateMgr = nullptr;
  });
  gDLL->registerStateFactories(gStateMgr);

  gWindow->setStateManager(gStateMgr);

  return options;
}

//
// Helper to set up debug text to display
//

void setupDebugPanel() {
  string fpsDisplay = "FPS: --";
  if (gStateMgr->isCurrentState("Play") &&
      !gGameFrameRate.samples().empty()) {
    auto const fps = gGameFrameRate.framesPerSecond();
    string fpsColour;
    if (fps < 30.0) {
      fpsColour = "[#FF0000FF]";
    } else if (fps < 55.0) {
      fpsColour = "[#FFFF00FF]";
    } else {
      fpsColour = "[#00FF00FF]";
    }
    fpsDisplay = std::format("FPS: {}{:.1f}", fpsColour, fps);
  }
  gRenderSystem->setDebugPreMessages({fpsDisplay});

  gRenderSystem->setDebugPostMessages(gStateMgr->getDebuggingText());

  gRenderSystem->showDebugPanel(gDisplayDebugEnabled,
                                mpp::RenderSystem::TimeUnit::Milliseconds,
                                mpp::RenderSystem::SizeUnit::Megabytes);
}

void updateImGui(float frameTime) {
  if (gStateMgr->imGuiActive()) {
    gWindow->showCursor(gStateMgr->imGuiCapturesInput());

    imGuiNewFrame();

    ImGuiIO& io = ImGui::GetIO();

    io.DeltaTime = frameTime;

    ImGui::NewFrame();

    // Pass the contexts across the DLL boundary
    auto imGuiCtx = ImGui::GetCurrentContext();
    auto imPlotCtx = ImPlot::GetCurrentContext();

    ImGuiMemAllocFunc imGuiAllocFunc;
    ImGuiMemFreeFunc imGuiFreeFunc;
    void* imGuiUserData;

    ImGui::GetAllocatorFunctions(
        &imGuiAllocFunc, &imGuiFreeFunc, &imGuiUserData);

    gStateMgr->renderImGui(
        frameTime, imGuiCtx, imPlotCtx,
        reinterpret_cast<void*>(imGuiAllocFunc),
        reinterpret_cast<void*>(imGuiFreeFunc), imGuiUserData);

    ImGui::EndFrame();
    ImGui::Render();

    gImGuiDataProvider->setDrawData(ImGui::GetDrawData());
  } else {
    gWindow->showCursor(false);
  }
}

//
// Entry point
//
int main(int argc, char** argv) {
  string configFile = "Game.yaml";
  if (argc > 1) {
    configFile = string(argv[1]);
  }

  LauncherLifecycle lifecycle;
  int exitCode = 0;
  uint64_t numFramesProcessed{0};
  double totalTime{0};
  int64_t totalTimeNs{0};
  try {
    auto options = startup(configFile, lifecycle);

    // Main loop
    float accum = 0.0f;
    const float updateFreq = 1.0f / 60.0f;

    mpp::RenderInfo renderInfo;

    gStateMgr->enterInitialState();
    gTimer->reset();

    bool trackingGameFrameRate = false;
    while (gWindow->isActive()) {
      // Get frame time
      float frameTime = gTimer->getDeltaTime();

      accum += frameTime;

      // Process window messages
      gWindow->processEvents(gStateMgr);

      // Update current state
      while (accum >= updateFreq) {
        accum -= updateFreq;

        updateImGui(updateFreq);

        auto captureCpuUpdateTimings =
            gDLL->cpuUpdateTimingCaptureEnabled();
        optional<wp::Timer> cpuUpdateTimer;
        if (captureCpuUpdateTimings) {
          cpuUpdateTimer.emplace();
        }
        auto startTime = elapsedSeconds();

        wp::Timer timerNs;
        auto startTimeNs = timerNs.elapsedNanoseconds();
        auto gameStartNs = captureCpuUpdateTimings
                               ? cpuUpdateTimer->elapsedNanoseconds()
                               : 0;

        gStateMgr->update(updateFreq);

        auto gameEndNs = captureCpuUpdateTimings
                             ? cpuUpdateTimer->elapsedNanoseconds()
                             : 0;
        int64_t audioUpdateNs = 0;
        if (gAudioSystem) {
          auto audioStartNs = captureCpuUpdateTimings
                                  ? cpuUpdateTimer->elapsedNanoseconds()
                                  : 0;
          gAudioSystem->update();
          if (captureCpuUpdateTimings) {
            audioUpdateNs =
                cpuUpdateTimer->elapsedNanoseconds() - audioStartNs;
          }
        }

        auto endTime = elapsedSeconds();
        auto endTimeNs = timerNs.elapsedNanoseconds();

        if (captureCpuUpdateTimings) {
          gDLL->recordCpuUpdateTimings(
              static_cast<uint64_t>(gameEndNs - gameStartNs),
              static_cast<uint64_t>(audioUpdateNs));
        }

        numFramesProcessed++;

        totalTime += (endTime - startTime);
        totalTimeNs += (endTimeNs - startTimeNs);
      }

      auto const gameStateActive =
          gStateMgr->isCurrentState("Play");
      if (gameStateActive && !trackingGameFrameRate) {
        // The first presentation interval now begins in Play; loading and
        // state-transition stalls can no longer enter the game FPS value.
        gGameFrameRate.reset();
        gDLL->resetFrameRateHistory();
      }
      trackingGameFrameRate = gameStateActive;

      // Render
      setupDebugPanel();

      gRenderSystem->startStatsCollection();

      gStateMgr->render(gRenderSystem, gRenderSystemResourceMgr);

      logWorldRenderGlState();  // [DEBUG-a4f2]

      auto ri = gRenderSystem->finishStatsCollection();

      if (gStateMgr->imGuiActive()) {
        gImGuiRenderer->render(gRenderSystem);
      }

      // The game requests F11 captures during its update. Fulfil them only
      // after launcher-owned overlays have been drawn so ImGui is included.
      gDLL->captureScreenshotIfRequested(gRenderSystem);

      captureFrameIfRequested();  // [DEBUG-a4f2]

      // Flip to screen. Sample afterwards so this interval includes the
      // driver's present/VSync wait rather than only CPU submission time.
      gWindow->show();
      if (trackingGameFrameRate && gStateMgr->isCurrentState("Play")) {
        auto const presentedAt = elapsedSeconds();
        gGameFrameRate.recordPresentedFrame(presentedAt);
        if (!gGameFrameRate.samples().empty()) {
          gDLL->recordFrameRate(
              presentedAt, gGameFrameRate.framesPerSecond());
        }
      }
    }
  } catch (ExitApplicationException& e) {
    if (auto average = LauncherLifecycle::averageDuration(totalTime, numFramesProcessed)) {
      gLogger->info(format("Avg update time ms: {}", *average * 1000.0));
      auto averageNs = LauncherLifecycle::averageDuration(static_cast<double>(totalTimeNs), numFramesProcessed);
      gLogger->info(format("Avg update time ms: {}", *averageNs / 1000000.0));
    } else {
      gLogger->info("No update frames processed.");
    }

    gLogger->info(e.getMessage());
    exitCode = e.getExitCode();
  } catch (application::resourcesystem::ResourceException& e) {
    if (auto const* resource = e.getResource()) {
      gLogger->error("Error in resource: " + resource->getQualifiedName());
    }
    gLogger->error(e.what());
    exitCode = 1;

#if defined(_DEBUG) && defined(_WIN32)
    char const* msg = e.what();

    size_t reqLength = ::MultiByteToWideChar(CP_UTF8, 0, msg, (int)strlen(msg), 0, 0);
    wstring ret(reqLength, L'\0');

    ::MultiByteToWideChar(CP_UTF8, 0, msg, (int)strlen(msg), &ret[0], (int)ret.length());
    OutputDebugString(ret.c_str());
#endif
  } catch (application::resourcesystem::ResourceSystemException& e) {
    gLogger->error(e.what());
    exitCode = 1;

#if defined(_DEBUG) && defined(_WIN32)
    char const* msg = e.what();

    size_t reqLength = ::MultiByteToWideChar(CP_UTF8, 0, msg, (int)strlen(msg), 0, 0);
    wstring ret(reqLength, L'\0');

    ::MultiByteToWideChar(CP_UTF8, 0, msg, (int)strlen(msg), &ret[0], (int)ret.length());
    OutputDebugString(ret.c_str());
#endif
  } catch (Exception& e) {
    gLogger->error(e.what());
    exitCode = 1;

#if defined(_DEBUG) && defined(_WIN32)
    char const* msg = e.what();

    size_t reqLength = ::MultiByteToWideChar(CP_UTF8, 0, msg, (int)strlen(msg), 0, 0);
    wstring ret(reqLength, L'\0');

    ::MultiByteToWideChar(CP_UTF8, 0, msg, (int)strlen(msg), &ret[0], (int)ret.length());
    OutputDebugString(ret.c_str());
#endif
  } catch (mpp::MppException& e) {
    // what() alone is useless here - MppGlException's message is just the bare
    // error name, e.g. "GL_INVALID_ENUM". The failing statement's location is
    // carried separately by the exception (GL_CHECK passes __LINE__/__FILE__/
    // __func__), so log that too or there is no way to find the call.
    auto detail = format("{} at {}:{} in {}\nstack trace: {}", e.what(), e.getFile(), e.getLine(),
                         e.getFunction(), e.getStackTrace());

    gLogger->error(detail);
    exitCode = 1;

#if defined(_DEBUG) && defined(_WIN32)
    char const* msg = detail.c_str();

    size_t reqLength = ::MultiByteToWideChar(CP_UTF8, 0, msg, (int)strlen(msg), 0, 0);
    wstring ret(reqLength, L'\0');

    ::MultiByteToWideChar(CP_UTF8, 0, msg, (int)strlen(msg), &ret[0], (int)ret.length());
    OutputDebugString(ret.c_str());
#endif
  } catch (exception& e) {
    gLogger->error(e.what());
    exitCode = 1;

#if defined(_DEBUG) && defined(_WIN32)
    char const* msg = e.what();

    size_t reqLength = ::MultiByteToWideChar(CP_UTF8, 0, msg, (int)strlen(msg), 0, 0);
    wstring ret(reqLength, L'\0');

    ::MultiByteToWideChar(CP_UTF8, 0, msg, (int)strlen(msg), &ret[0], (int)ret.length());
    OutputDebugString(ret.c_str());
#endif
  }

  lifecycle.teardown([](string_view service, exception_ptr error) {
    try {
      rethrow_exception(error);
    } catch (exception const& e) {
      if (gLogger) {
        gLogger->error(format("Error tearing down {}: {}", service, e.what()));
      }
    } catch (...) {
      if (gLogger) {
        gLogger->error(format("Unknown error tearing down {}", service));
      }
    }
  });
  return exitCode;
}