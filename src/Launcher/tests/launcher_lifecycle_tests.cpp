#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "FrameRateTracker.h"
#include "LauncherLifecycle.h"

namespace {
using Service = LauncherLifecycle::Service;

constexpr Service startupOrder[]{
    Service::Logger,
    Service::MppLogger,
    Service::ApplicationSettings,
    Service::Platform,
    Service::Timer,
    Service::Window,
    Service::RenderSystem,
    Service::RenderResourceManager,
    Service::RenderCoreResources,
    Service::AudioSystem,
    Service::ResourceManager,
    Service::ImGuiContext,
    Service::ImPlotContext,
    Service::ImGuiBackend,
    Service::ImGuiDataProvider,
    Service::ImGuiRenderer,
    Service::ApplicationDll,
    Service::StateManager,
};

constexpr Service teardownOrder[]{
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

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

void verifyFrameRateTracking() {
  FrameRateTracker tracker;
  tracker.recordPresentedFrame(100.0);
  require(tracker.framesPerSecond() == 0.0 && tracker.samples().empty(),
          "A single presented frame produced an FPS value");

  constexpr double frameDuration = 1.0 / 60.0;
  for (int frame = 1; frame <= 360; ++frame) {
    tracker.recordPresentedFrame(100.0 + frame * frameDuration);
  }

  require(std::abs(tracker.framesPerSecond() - 60.0) < 0.001,
          "Presented-frame FPS was calculated incorrectly");
  require(!tracker.samples().empty() &&
              tracker.samples().back().timestampSeconds -
                      tracker.samples().front().timestampSeconds <=
                  5.0 + frameDuration,
          "Frame-rate graph retained more than five seconds");

  tracker.reset();
  require(tracker.framesPerSecond() == 0.0 && tracker.samples().empty(),
          "Reset retained frame-rate state");
}

void verifyFailureAfter(size_t lastConstructed) {
  LauncherLifecycle lifecycle;
  std::vector<Service> destroyed;
  std::vector<int> destructionCounts(static_cast<size_t>(Service::Count));

  for (size_t i = 0; i <= lastConstructed; ++i) {
    auto service = startupOrder[i];
    lifecycle.track(service, [&, service]() {
      destroyed.push_back(service);
      ++destructionCounts[static_cast<size_t>(service)];
    });
  }

  lifecycle.teardown();
  lifecycle.teardown();

  std::vector<Service> expected;
  for (auto service : teardownOrder) {
    if (std::find(startupOrder, startupOrder + lastConstructed + 1, service) !=
        startupOrder + lastConstructed + 1) {
      expected.push_back(service);
    }
  }

  require(destroyed == expected,
          "Incorrect teardown order after failure at " +
              std::string(LauncherLifecycle::name(startupOrder[lastConstructed])));
  for (size_t i = 0; i < destructionCounts.size(); ++i) {
    auto wasConstructed =
        std::find(startupOrder, startupOrder + lastConstructed + 1, static_cast<Service>(i)) !=
        startupOrder + lastConstructed + 1;
    require(destructionCounts[i] == (wasConstructed ? 1 : 0),
            "Service was not conditionally destroyed exactly once");
  }
}
}  // namespace

int main() {
  try {
    // Inject a startup failure after every phase, including fully constructed
    // startup, and verify the same dependency-aware teardown used by Launcher.
    for (size_t phase = 0; phase < std::size(startupOrder); ++phase) {
      verifyFailureAfter(phase);
    }

    require(!LauncherLifecycle::averageDuration(1.0, 0).has_value(),
            "Zero-frame timing produced an average");
    require(LauncherLifecycle::averageDuration(3.0, 2).value() == 1.5,
            "Non-zero timing average was incorrect");

    verifyFrameRateTracking();

    std::cout << "Launcher lifecycle and frame-rate tests passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
