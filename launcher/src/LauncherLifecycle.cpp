#include "LauncherLifecycle.h"

#include <stdexcept>

namespace {
using Service = LauncherLifecycle::Service;

// Teardown is dependency order, not simply reverse construction order. In
// particular, states and resource factories can execute code from the game DLL.
//
// RenderCoreResources/RenderResourceManager/RenderSystem come before
// ApplicationDll, not after: the render system's long-lived caches
// (RenderSystem::mPipelines, ResourceManager's resource cache, the pooled
// MeshInstances RenderSystem owns) can end up holding the last shared_ptr
// to an object whose most-derived type - or whose members, like a
// UniformCollection allocated inside BooleanWorld.dll's own instrumented
// heap - only exist while BooleanWorld.dll is loaded. Destroying those
// caches after ApplicationDll has already unloaded the DLL crashes when
// that reference is finally released. Tearing the render system down
// first, while the DLL is still mapped, avoids the whole class of bug
// regardless of which object happens to be the last one holding a
// reference.
constexpr std::array teardownOrder{
    Service::StateManager,
    Service::ImGuiRenderer,
    Service::ImGuiDataProvider,
    Service::ImGuiBackend,
    Service::ImPlotContext,
    Service::ImGuiContext,
    Service::ResourceManager,
    Service::RenderCoreResources,
    Service::RenderResourceManager,
    Service::RenderSystem,
    Service::ApplicationDll,
    Service::AudioSystem,
    Service::Window,
    Service::Timer,
    Service::Platform,
    Service::ApplicationSettings,
    Service::MppLogger,
    Service::Logger,
};

constexpr std::array serviceNames{
    std::string_view{"logger"},
    std::string_view{"MPP logger"},
    std::string_view{"application settings"},
    std::string_view{"windowing platform"},
    std::string_view{"timer"},
    std::string_view{"window"},
    std::string_view{"render system"},
    std::string_view{"render resource manager"},
    std::string_view{"render core resources"},
    std::string_view{"audio system"},
    std::string_view{"resource manager"},
    std::string_view{"ImGui context"},
    std::string_view{"ImPlot context"},
    std::string_view{"ImGui backend"},
    std::string_view{"ImGui data provider"},
    std::string_view{"ImGui renderer"},
    std::string_view{"application DLL"},
    std::string_view{"state manager"},
};

static_assert(teardownOrder.size() == static_cast<size_t>(Service::Count));
static_assert(serviceNames.size() == static_cast<size_t>(Service::Count));
}  // namespace

LauncherLifecycle::~LauncherLifecycle() {
  teardown();
}

void LauncherLifecycle::track(Service service, Cleanup cleanup) {
  auto& slot = mCleanups.at(static_cast<size_t>(service));
  if (slot) {
    throw std::logic_error("Launcher service tracked more than once");
  }
  slot = std::move(cleanup);
}

void LauncherLifecycle::teardown(ErrorHandler const& onError) noexcept {
  for (auto service : teardownOrder) {
    auto& slot = mCleanups[static_cast<size_t>(service)];
    if (!slot) {
      continue;
    }

    // Remove the callback before invoking it so re-entry and exceptions cannot
    // destroy a partially constructed service twice.
    auto cleanup = std::move(slot);
    slot = {};
    try {
      cleanup();
    } catch (...) {
      if (onError) {
        try {
          onError(name(service), std::current_exception());
        } catch (...) {
          // Teardown must continue even if error reporting itself fails.
        }
      }
    }
  }
}

std::string_view LauncherLifecycle::name(Service service) {
  return serviceNames.at(static_cast<size_t>(service));
}

std::optional<double> LauncherLifecycle::averageDuration(double total, uint64_t sampleCount) {
  if (sampleCount == 0) {
    return std::nullopt;
  }
  return total / static_cast<double>(sampleCount);
}
