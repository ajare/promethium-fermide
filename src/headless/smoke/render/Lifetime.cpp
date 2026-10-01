#include "Checks.h"
#include "ImGuiContext.h"
// World render lifetime, for ticket #199.
//
// renderWorld() used to publish the rendered document to a process-global
// shared_ptr and never release it. Closing the last editor document therefore
// left the whole World alive until another World was rendered or the process
// exited, and a World that depends on unregistering from shared Agent tag and
// behaviour registries stayed registered while nobody was rendering it.
//
// The render context is now a RenderWorldScope. It owns the World for exactly
// one render and restores whatever was published before, on every exit path,
// so a transient render context can never become a second document owner.
//
// These checks pin the contract:
//
//   * releasing the last external shared_ptr after renderWorld() destroys the
//     World immediately (a weak_ptr expires) - the ticket's probe;
//   * an early return (no draw list) is equally leak-free;
//   * a closed World unregisters from an attached registry without waiting for
//     another render;
//   * a nested RenderWorldScope publishes its own World and restores the
//     previous one when it ends, which the aperture pass reads back;
//   * the nested Background/aperture pass still receives the World
//     renderWorld() installed.
//
// Everything runs headless: ImGui is created without a renderer, so no window,
// dialog, or GPU is ever touched.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "imgui/imgui.h"

#include "Render.h"
#include "UISettings.h"
#include "WorldDrawList.h"
#include "core/AgentTagRegistry.h"
#include "core/Background.h"
#include "core/Defines.h"
#include "core/Window.h"
#include "core/World.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// ImGui without a renderer: contexts are CPU-side only, nothing reaches a
	// window or the GPU.
	using ImGuiGuard = headless::ScopedImGuiContext;

	// A viewport covering the whole fixture, unzoomed and unscrolled.
	void setViewport()
	{
		gUISettings = UISettings{};
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 6 * CORE_CELL_WIDTH_PIXELS;
		gUISettings.worldViewportHeight = 2 * CORE_LEVEL_HEIGHT_PIXELS;
		gUISettings.xOffset = 0.0f;
		gUISettings.yOffset = 0.0f;
		gUISettings.renderGrid = false;
		gUISettings.renderNextLayerWireframe = false;
	}

	WorldDrawList makeDraws()
	{
		return WorldDrawList(WorldDrawList::ClipRectangle{ { 0, 0 },
			{ gUISettings.worldViewportWidth, gUISettings.worldViewportHeight } });
	}

	// Sample the painter-ordered command stream, respecting nested scissor
	// rectangles. These scenes use opaque untextured fills and sample away from
	// outlines and text.
	ImU32 colourAt(WorldDrawList const& draws, ImVec2 point)
	{
		ImU32 result = 0;
		for (auto const& command : draws.commands())
		{
			auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle) continue;
			auto const& clip = triangle->clip;
			if (point.x < clip.minimum.x || point.x >= clip.maximum.x
				|| point.y < clip.minimum.y || point.y >= clip.maximum.y) continue;
			auto cross = [point](ImVec2 a, ImVec2 b)
				{ return (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x); };
			auto const a = cross(triangle->positions[0], triangle->positions[1]);
			auto const b = cross(triangle->positions[1], triangle->positions[2]);
			auto const c = cross(triangle->positions[2], triangle->positions[0]);
			if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
				result = triangle->colour;
		}
		return result;
	}

	// A front Room with a clear Window onto a Background one Layer behind. The
	// colour the Window reveals is read back through the World the aperture
	// pass sees, so rendering it proves which World is installed.
	struct ApertureScene
	{
		std::shared_ptr<core::World> world;
		uint32_t frontRoom{};
		std::shared_ptr<const core::Window> window;

		ApertureScene(std::string name, core::BackgroundColour backgroundColour)
			: world(std::make_shared<core::World>(std::move(name), 6, 2))
		{
			frontRoom = world->addRoom("Front", 0, 0, 0, 6, 1);
			world->addBackground(1, 0, 0, 6, 1, backgroundColour);
			window = world->addSectorWindow(0, 0, 1, 3, 1,
				{ false, core::Window::State::Closed, core::Window::Style::Clear }).object;
			world->finishBuild();
		}

		// The screen point the Window reveals the Background through.
		ImVec2 revealedPoint() const
		{
			require(window != nullptr, "the aperture fixture has no Window");
			core::Vector2 windowMin, windowMax;
			window->getFullShape(windowMin, windowMax);
			return { (windowMin.x + windowMax.x) * 0.5f * CORE_CELL_WIDTH_PIXELS,
				gUISettings.worldViewportHeight
					- (windowMin.y + windowMax.y) * 0.5f * CORE_LEVEL_HEIGHT_PIXELS };
		}
	};
}

// The ticket's probe: the final external shared_ptr release must destroy a
// rendered World immediately, not leave it owned by the render context.
void renderedWorldIsDestroyedOnFinalRelease()
{
	ImGuiGuard imgui;
	setViewport();

	auto world = std::make_shared<core::World>("Render lifetime", 4, 2);
	world->addRoom("Room", 0, 0, 0, 4, 1);
	world->finishBuild();

	std::weak_ptr<core::World> weak = world;
	auto draws = makeDraws();
	renderWorld(world, &draws);
	require(!weak.expired(), "the World must stay alive while the caller holds it");

	world.reset();
	require(weak.expired(),
		"renderWorld() kept the last closed World alive through its render context (#199)");
}

// A render that bails out before drawing anything must not retain the World
// either: the old context was published before the early return.
void earlyReturnDoesNotRetainTheWorld()
{
	auto world = std::make_shared<core::World>("Render lifetime early", 4, 2);
	world->addRoom("Room", 0, 0, 0, 4, 1);
	world->finishBuild();

	std::weak_ptr<core::World> weak = world;
	renderWorld(world, nullptr);

	world.reset();
	require(weak.expired(),
		"an early-returning render left the World installed in the render context (#199)");
}

// Closing a document must unregister it from attached registries at once,
// rather than waiting for the next render to release the last reference.
void closedWorldUnregistersFromItsRegistry()
{
	ImGuiGuard imgui;
	setViewport();

	auto registry = core::AgentTagRegistry::create();
	auto world = std::make_shared<core::World>("Render lifetime registry", 4, 2);
	world->attachAgentTagRegistry("lifetime.tags.yaml", registry);
	world->addRoom("Room", 0, 0, 0, 4, 1);
	world->finishBuild();
	require(registry->hasLoadedWorlds(), "the attached registry did not register the World");

	auto draws = makeDraws();
	renderWorld(world, &draws);

	world.reset();
	require(!registry->hasLoadedWorlds(),
		"a closed World stayed registered until the next render (#199)");
}

// The aperture pass reads the World back from the render context. Rendering
// through renderWorld() must still reveal the Background behind the Window.
void nestedApertureSeesTheRenderedWorld()
{
	ImGuiGuard imgui;
	setViewport();

	ApertureScene scene("Render lifetime aperture", { 23, 67, 109 });
	auto draws = makeDraws();
	renderWorld(scene.world, &draws);

	require(colourAt(draws, scene.revealedPoint()) == ImU32(ImColor(23, 67, 109)),
		"the nested aperture pass did not receive the World renderWorld() installed (#199)");
}

// A nested scope publishes its own World for its own render and restores the
// previous one afterwards. Both renders read the aperture pass back, so a
// wrong or stale World changes which Background colour the Window reveals.
void nestedScopesRestoreThePreviousWorld()
{
	ImGuiGuard imgui;
	setViewport();

	ApertureScene outer("Render lifetime outer", { 23, 67, 109 });
	ApertureScene inner("Render lifetime inner", { 151, 37, 83 });

	{
		RenderWorldScope outerScope(outer.world);

		{
			RenderWorldScope innerScope(inner.world);
			auto innerDraws = makeDraws();
			renderSector(inner.world->getSector(inner.frontRoom), 0,
				LayerRenderStyle::Solid, false, ImColor(192, 192, 255), &innerDraws);
			require(colourAt(innerDraws, inner.revealedPoint()) == ImU32(ImColor(151, 37, 83)),
				"a nested RenderWorldScope did not publish its own World (#199)");
		}

		auto outerDraws = makeDraws();
		renderSector(outer.world->getSector(outer.frontRoom), 0,
			LayerRenderStyle::Solid, false, ImColor(192, 192, 255), &outerDraws);
		require(colourAt(outerDraws, outer.revealedPoint()) == ImU32(ImColor(23, 67, 109)),
			"a nested RenderWorldScope did not restore the previous World (#199)");
	}
}


namespace
{
	void scopedContextRestoresStateAfterFailure(smoke::Context const& context)
	{
		auto* previous = ImGui::GetCurrentContext();
		gUISettings.worldZoom = 1.75f;
		{
			headless::ScopedImGuiContext nested;
			require(ImGui::GetCurrentContext() != previous, "Scoped context reused its caller's context");
			require(ImGui::GetIO().IniFilename == nullptr && ImGui::GetIO().LogFilename == nullptr,
				"Headless context permits persistent ImGui output");
		}
		require(ImGui::GetCurrentContext() == previous, "Normal scope did not restore the caller's context");

		bool caught = false;
		try
		{
			render_smoke::isolated<[](smoke::Context const&)
			{
				gUISettings.worldZoom = 3.0f;
				headless::ScopedImGuiContext nested;
				throw std::runtime_error("expected render failure");
			}>(context);
		}
		catch (std::runtime_error const&)
		{
			caught = true;
		}
		require(caught, "Render failure did not propagate to the registered-check boundary");
		require(ImGui::GetCurrentContext() == previous, "Exceptional scope did not restore the caller's context");
		require(gUISettings.worldZoom == 1.75f, "Exceptional render check leaked UI settings");
	}
}

void render_smoke::registerLifetime(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "scopedContextRestoresStateAfterFailure", isolated<scopedContextRestoresStateAfterFailure> });
	checks.push_back({ "renderedWorldIsDestroyedOnFinalRelease", isolated<[](smoke::Context const&) { renderedWorldIsDestroyedOnFinalRelease(); }> });
	checks.push_back({ "earlyReturnDoesNotRetainTheWorld", isolated<[](smoke::Context const&) { earlyReturnDoesNotRetainTheWorld(); }> });
	checks.push_back({ "closedWorldUnregistersFromItsRegistry", isolated<[](smoke::Context const&) { closedWorldUnregistersFromItsRegistry(); }> });
	checks.push_back({ "nestedApertureSeesTheRenderedWorld", isolated<[](smoke::Context const&) { nestedApertureSeesTheRenderedWorld(); }> });
	checks.push_back({ "nestedScopesRestoreThePreviousWorld", isolated<[](smoke::Context const&) { nestedScopesRestoreThePreviousWorld(); }> });
}
