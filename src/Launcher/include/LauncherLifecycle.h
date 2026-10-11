#pragma once

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string_view>

class LauncherLifecycle {
public:
  enum class Service {
    Logger,
    MppLogger,
    ApplicationSettings,
    Platform,
    Timer,
    Window,
    RenderSystem,
    RenderResourceManager,
    RenderCoreResources,
    AudioSystem,
    ResourceManager,
    ImGuiContext,
    ImPlotContext,
    ImGuiBackend,
    ImGuiDataProvider,
    ImGuiRenderer,
    ApplicationDll,
    StateManager,
    Count
  };

  using Cleanup = std::function<void()>;
  using ErrorHandler = std::function<void(std::string_view, std::exception_ptr)>;

  LauncherLifecycle() = default;
  LauncherLifecycle(LauncherLifecycle const&) = delete;
  LauncherLifecycle& operator=(LauncherLifecycle const&) = delete;
  ~LauncherLifecycle();

  void track(Service service, Cleanup cleanup);
  void teardown(ErrorHandler const& onError = {}) noexcept;

  static std::string_view name(Service service);
  static std::optional<double> averageDuration(double total, uint64_t sampleCount);

private:
  static constexpr size_t serviceCount = static_cast<size_t>(Service::Count);
  std::array<Cleanup, serviceCount> mCleanups;
};
