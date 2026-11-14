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
#include "SectorDrawSheet.h"
#include "SectorTileset.h"
#include "UISettings.h"
#include "WorldDrawList.h"
#include "WorldToScreen.h"
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

	// The topmost triangle painted over a point, respecting nested scissor
	// rectangles. Nothing in these scenes is a flat colour any more, so the
	// answer is the triangle itself and the caller reads what it says.
	WorldDrawList::Triangle const* topAt(WorldDrawList const& draws, Vec2 point)
	{
		WorldDrawList::Triangle const* result = nullptr;
		for (auto const& command : draws.commands())
		{
			auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle) continue;
			auto const& clip = triangle->clip;
			if (point.x < clip.minimum.x || point.x >= clip.maximum.x
				|| point.y < clip.minimum.y || point.y >= clip.maximum.y) continue;
			auto cross = [point](Vec2 a, Vec2 b)
				{ return (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x); };
			auto const a = cross(triangle->positions[0], triangle->positions[1]);
			auto const b = cross(triangle->positions[1], triangle->positions[2]);
			auto const c = cross(triangle->positions[2], triangle->positions[0]);
			if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
				result = triangle;
		}
		return result;
	}

	// A sheet with one full-cell tile per surface these scenes use, each in
	// its own column, so a quad's tile is readable back out of the UVs it
	// sampled. Which tile the aperture drew says which Layer it looked
	// through to, which is what these checks are really asking.
	SectorTileset installFlatSurfaceAtlas()
	{
		SectorTileset atlas;
		atlas.family = SectorTilesetFamily::New;
		int column = 0;
		for (auto const* name : { "room", "background" })
			atlas.surfaces.emplace(name, SectorTileRegion{
				column++ * CORE_CELL_WIDTH_PIXELS, 0,
				CORE_CELL_WIDTH_PIXELS, CORE_LEVEL_HEIGHT_PIXELS });
		atlas.width = column * CORE_CELL_WIDTH_PIXELS;
		atlas.height = CORE_LEVEL_HEIGHT_PIXELS;
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(drawingSectorSheet()),
			"The synthetic lifetime atlas is not the New family");
		return atlas;
	}

	std::string tileOf(WorldDrawList::Triangle const* triangle,
		SectorTileset const& atlas)
	{
		if (!triangle || triangle->texture != WorldDrawList::Texture::SectorAtlas)
			return {};
		float u0{ 1e9f }, u1{ -1e9f }, v0{ 1e9f }, v1{ -1e9f };
		for (auto const& uv : triangle->texcoords)
		{
			u0 = std::min(u0, uv.x); u1 = std::max(u1, uv.x);
			v0 = std::min(v0, uv.y); v1 = std::max(v1, uv.y);
		}
		auto const cu = (u0 + u1) * 0.5f;
		auto const cv = (v0 + v1) * 0.5f;
		for (auto const& [name, region] : atlas.surfaces)
		{
			auto const left = float(region.x) / float(atlas.width);
			auto const right = float(region.x + region.width) / float(atlas.width);
			auto const top = float(region.y) / float(atlas.height);
			auto const bottom = float(region.y + region.height) / float(atlas.height);
			if (cu >= left && cu < right && cv >= top && cv < bottom)
				return name;
		}
		return {};
	}

	bool showsTileAt(WorldDrawList const& draws, Vec2 point,
		SectorTileset const& atlas, std::string const& want)
	{
		return tileOf(topAt(draws, point), atlas) == want;
	}

	// A front Room with a clear Window onto a Background one Layer behind. The
	// colour the Window reveals is read back through the World the aperture
	// pass sees, so rendering it proves which World is installed.
	struct ApertureScene
	{
		std::shared_ptr<core::World> world;
		uint32_t frontRoom{};
		std::shared_ptr<const core::Window> window;

		// `windowCell` is where the Window sits on the front Room. Two
		// scenes with their Windows in different cells cannot be confused
		// for one another once neither paints a colour of its own: what one
		// reveals at a point, the other does not.
		ApertureScene(std::string name, int windowCell)
			: world(std::make_shared<core::World>(std::move(name), 6, 2))
		{
			frontRoom = world->addRoom("Front", 0, 0, 0, 6, 1);
			world->addBackground(1, 0, 0, 6, 1, { 23, 67, 109 });
			window = world->addSectorWindow(0, 0, windowCell, 3, 1,
				{ false, core::Window::State::Closed, core::Window::Style::Clear }).object;
			world->finishBuild();
		}

		// The screen point the Window reveals the Background through, put
		// there by the render module's shared mapping rather than by the
		// arithmetic restated here (#1100).
		Vec2 revealedPoint() const
		{
			require(window != nullptr, "the aperture fixture has no Window");
			core::Vector2 windowMin, windowMax;
			window->getFullShape(windowMin, windowMax);
			return pf::render::worldToScreen(gUISettings).toScreen(
				{(windowMin.x + windowMax.x) * 0.5f,
					(windowMin.y + windowMax.y) * 0.5f});
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

	auto const atlas = installFlatSurfaceAtlas();
	ApertureScene scene("Render lifetime aperture", 1);
	auto draws = makeDraws();
	renderWorld(scene.world, &draws);

	require(showsTileAt(draws, scene.revealedPoint(), atlas, "background"),
		"the nested aperture pass did not receive the World renderWorld() installed (#199)");
}

// A nested scope publishes its own World for its own render and restores the
// previous one afterwards. Both renders read the aperture pass back, and
// the two scenes put their Windows in different cells, so a wrong or stale
// World reveals the Background somewhere the rendered World does not.
void nestedScopesRestoreThePreviousWorld()
{
	ImGuiGuard imgui;
	setViewport();

	auto const atlas = installFlatSurfaceAtlas();
	ApertureScene outer("Render lifetime outer", 1);
	ApertureScene inner("Render lifetime inner", 3);

	{
		RenderWorldScope outerScope(outer.world);

		{
			RenderWorldScope innerScope(inner.world);
			auto innerDraws = makeDraws();
			renderSector(inner.world->getSector(inner.frontRoom), 0,
				LayerRenderStyle::Solid, false, ImColor(192, 192, 255), &innerDraws);
			require(showsTileAt(innerDraws, inner.revealedPoint(), atlas, "background"),
				"a nested RenderWorldScope did not publish its own World (#199)");
		}

		auto outerDraws = makeDraws();
		renderSector(outer.world->getSector(outer.frontRoom), 0,
			LayerRenderStyle::Solid, false, ImColor(192, 192, 255), &outerDraws);
		require(showsTileAt(outerDraws, outer.revealedPoint(), atlas, "background"),
			"a nested RenderWorldScope did not restore the previous World (#199)");
		require(!showsTileAt(outerDraws, inner.revealedPoint(), atlas, "background"),
			"the restored render still saw the inner World's Window");
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
