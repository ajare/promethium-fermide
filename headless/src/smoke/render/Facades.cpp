#include "Checks.h"
#include "ImGuiContext.h"
// Facade and Background surface rendering.
//
// A Facade draws a tile of its own family and so does a Background, on the
// same terms as a Room or a Corridor. Neither is filled with a colour the
// renderer packs from the Sector's authored triple any more: their surface
// is artwork the installed atlas carries, drawn exactly as the sheet
// paints it. What the authored colour still *is* stays a domain question;
// what it is no longer is paint.
//
// The properties pinned down:
//
//   * the Solid pass tiles a Facade with the `facade` tile across its
//     whole area, and nothing else reaches that area;
//   * a Background tiles with `background` the same way;
//   * the wireframe overlay contributes the outline only, never a fill
//     (ADR 0002);
//   * a Sector's lights-off state changes no picture, because nothing is
//     multiplied into a tile on the way to the screen;
//   * a Window looking into a Facade still shows the Facade's objects and
//     its agents;
//   * a Window spanning a Facade and a Background is still refused at
//     authoring, so the aperture never has to composite the two.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Render.h"
#include "SectorDrawSheet.h"
#include "SectorTileset.h"
#include "UISettings.h"
#include "WorldDrawList.h"
#include "core/Background.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/Facade.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/Vector2.h"
#include "core/Window.h"

// The viewport the renderer reads, defined by the application.
extern UISettings gUISettings;

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

	int const kCellWidth = CORE_CELL_WIDTH_PIXELS;
	int const kCellHeight = CORE_LEVEL_HEIGHT_PIXELS;

	// A Facade colour and a Background colour that differ from every Layer
	// colour in the renderer, so a picture that still followed one of them
	// could not be mistaken for the artwork.
	core::BackgroundColour const kFacadeColour{ 210, 66, 138 };
	core::BackgroundColour const kBackgroundColour{ 12, 200, 66 };

	// One full-cell tile per flat surface, each in its own column of a
	// one-row sheet, so no two tiles share a texel column and a quad's
	// tile is readable back out of the UVs it sampled.
	SectorTileset installFlatSurfaceAtlas()
	{
		SectorTileset atlas;
		atlas.family = SectorTilesetFamily::New;
		int column = 0;
		for (auto const* name : { "room", "facade", "background" })
			atlas.surfaces.emplace(name, SectorTileRegion{
				column++ * kCellWidth, 0, kCellWidth, kCellHeight });
		atlas.width = column * kCellWidth;
		atlas.height = kCellHeight;
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(drawingSectorSheet()),
			"The synthetic flat-surface atlas did not come up as the New family");
		return atlas;
	}

	void pinViewport()
	{
		gUISettings = UISettings{};
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 4096.0f;
		gUISettings.worldViewportHeight = 4096.0f;
		gUISettings.xOffset = 0.0f;
		gUISettings.yOffset = 0.0f;
		gUISettings.worldZoom = 1.0f;
	}

	WorldDrawList::ClipRectangle const kNoClip{ { -100000.0f, -100000.0f },
		{ 100000.0f, 100000.0f } };

	// The tile one UV centre falls on.
	std::string tileAt(float centreU, float centreV, SectorTileset const& atlas)
	{
		for (auto const& [name, region] : atlas.surfaces)
		{
			auto const left = float(region.x) / float(atlas.width);
			auto const right = float(region.x + region.width) / float(atlas.width);
			auto const top = float(region.y) / float(atlas.height);
			auto const bottom = float(region.y + region.height) / float(atlas.height);
			if (centreU >= left && centreU < right && centreV >= top
					&& centreV < bottom)
				return name;
		}
		return {};
	}

	// The textured quads one pass emitted, as tile names.
	std::vector<std::string> drawnTiles(WorldDrawList const& drawing,
		SectorTileset const& atlas)
	{
		std::vector<std::string> tiles;
		for (auto const& command : drawing.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::SectorAtlas)
				continue;
			float u0{ 1e9f }, u1{ -1e9f }, v0{ 1e9f }, v1{ -1e9f };
			for (auto const& uv : triangle->texcoords)
			{
				u0 = std::min(u0, uv.x); u1 = std::max(u1, uv.x);
				v0 = std::min(v0, uv.y); v1 = std::max(v1, uv.y);
			}
			tiles.push_back(tileAt((u0 + u1) * 0.5f, (v0 + v1) * 0.5f, atlas));
		}
		return tiles;
	}

	int countTile(std::vector<std::string> const& tiles, std::string const& want)
	{
		return int(std::count(tiles.begin(), tiles.end(), want));
	}

	// Every textured quad the pass emitted, whatever atlas it sampled.
	std::vector<WorldDrawList::Triangle> texturedQuads(
		WorldDrawList const& drawing)
	{
		std::vector<WorldDrawList::Triangle> out;
		for (auto const& command : drawing.commands())
			if (auto const* t = std::get_if<WorldDrawList::Triangle>(&command);
				t && t->texture != WorldDrawList::Texture::None)
				out.push_back(*t);
		return out;
	}

	int lineCount(WorldDrawList const& drawing)
	{
		int lines = 0;
		for (auto const& command : drawing.commands())
			if (std::holds_alternative<WorldDrawList::Line>(command)) ++lines;
		return lines;
	}

	WorldDrawList renderFlat(std::shared_ptr<const core::Sector> const& sector,
		LayerRenderStyle style)
	{
		WorldDrawList drawing{ kNoClip };
		renderSector(sector, sector->getLayerIndex(), style, false,
			packColour(192, 192, 255), &drawing);
		return drawing;
	}

	//
	// A two-Layer frontage: a Room up front with a clear Window, a Facade
	// directly behind it, so the Window's span sits wholly inside the
	// Facade.
	//
	//   Layer 0  Front Room (Window authored here)
	//   Layer 1  Frontage Facade at x=2..5
	//
	struct Frontage
	{
		core::World world{ "Facade frontage", 12, 2 };
		uint32_t frontIndex{ 0 };
		uint32_t facadeIndex{ 0 };
		std::shared_ptr<const core::Facade> facade;
		std::shared_ptr<const core::Window> window;

		// World is not copyable (it owns unique_ptrs), so the whole
		// frontage is authored in place rather than returned from a helper.
		Frontage()
		{
			while (world.getLayerCount() < 2) world.addLayer();

			frontIndex = world.addRoom("Front", 0, 0, 0, 12, 1);
			facadeIndex = world.addFacade("Frontage", 1, 0, 2, 4, 1,
				CORE_ROOM_MAX_HEIGHT, kFacadeColour);

			std::string diagnostic;
			require(world.canAddSectorWindow(0, 0, 3, 2, 1, &diagnostic),
				("a Window looking into a Facade was refused: " + diagnostic).c_str());

			auto created = world.addSectorWindow(0, 0, 3, 2, 1,
				{ false, core::Window::State::Closed, core::Window::Style::Clear });
			window = created.object;
			require(window != nullptr, "the Window into the Facade was not created");
			require(!created.traversalResource,
				"a looking Window into a Facade was given a traversal resource");

			auto sector = world.getSector(facadeIndex);
			require(sector != nullptr && sector->getType() == core::SectorType::Facade,
				"the Sector behind the Window is not a Facade");
			facade = std::static_pointer_cast<const core::Facade>(sector);
		}
	};

	//
	// The Facade's surface is its own tile, laid one per World cell across
	// its whole area like any wall's, and drawn with no colour multiplied
	// into it.
	//
	void aFacadeDrawsItsOwnTileAcrossItsArea()
	{
		auto const atlas = installFlatSurfaceAtlas();
		pinViewport();

		Frontage const f;
		auto const drawing = renderFlat(f.facade, LayerRenderStyle::Solid);
		auto const tiles = drawnTiles(drawing, atlas);

		require(countTile(tiles, "facade") >= 2,
			"a Facade four cells wide drew no `facade` tile across its area");
		require(std::none_of(tiles.begin(), tiles.end(),
				[](std::string const& tile) { return tile != "facade"; }),
			"a Facade drew a tile that is not its own");
		for (auto const& quad : texturedQuads(drawing))
			require(quad.colour == White,
				"a Facade tile was drawn with a colour other than its raw artwork");
	}

	//
	// A Background is a surface like any other: it asks for `background`
	// and draws what that tile shows.
	//
	void aBackgroundDrawsItsOwnTileAcrossItsArea()
	{
		auto const atlas = installFlatSurfaceAtlas();
		pinViewport();

		core::World world("Background surface", 12, 1);
		auto const index = world.addBackground(0, 0, 0, 4, 1, kBackgroundColour);
		world.finishBuild();
		auto const background = world.getSector(index);
		require(background != nullptr
				&& background->getType() == core::SectorType::Background,
			"the fixture Sector is not a Background");

		auto const drawing = renderFlat(background, LayerRenderStyle::Solid);
		auto const tiles = drawnTiles(drawing, atlas);
		require(countTile(tiles, "background") >= 2,
			"a Background four cells wide drew no `background` tile across its area");
		require(std::none_of(tiles.begin(), tiles.end(),
				[](std::string const& tile) { return tile != "background"; }),
			"a Background drew a tile that is not its own");
	}

	//
	// The wireframe overlay contributes the outline only, never a fill
	// (ADR 0002): a fill from the Layer behind would bleed over the
	// selection.
	//
	void aFacadeIsOutlinedByTheWireframeOverlayAndFillsNothing()
	{
		auto const atlas = installFlatSurfaceAtlas();
		pinViewport();

		Frontage const f;
		auto const drawing = renderFlat(f.facade, LayerRenderStyle::Wireframe);
		require(texturedQuads(drawing).empty(),
			"the wireframe overlay filled a Facade instead of outlining it");
		require(lineCount(drawing) > 0,
			"the wireframe overlay drew no outline for a Facade");

		// Hidden paints nothing at all.
		require(renderFlat(f.facade, LayerRenderStyle::Hidden).commands().empty(),
			"the Hidden style paints a Facade");
	}

	//
	// A Sector's lights-off state changes no picture. Nothing is
	// multiplied into a tile, so the stream a Facade produces with its
	// lights off is the stream it produced with them on - the same tiles,
	// in the same order, with the same colours.
	//
	void aFacadeDrawsTheSamePictureWithItsLightsOff()
	{
		auto const atlas = installFlatSurfaceAtlas();
		pinViewport();

		Frontage const f;
		require(f.facade->areLightsOn(),
			"a freshly built Facade starts with its lights off");

		auto const on = renderFlat(f.facade, LayerRenderStyle::Solid);
		std::const_pointer_cast<core::Facade>(f.facade)->lightsOff();
		require(!f.facade->areLightsOn(), "the Facade's lights did not go off");
		auto const off = renderFlat(f.facade, LayerRenderStyle::Solid);

		require(on.commands().size() == off.commands().size(),
			"turning a Facade's lights off changed how much it draws");
		auto const onTiles = drawnTiles(on, atlas);
		auto const offTiles = drawnTiles(off, atlas);
		require(onTiles == offTiles,
			"a Facade with its lights off drew different tiles than with them on");
		for (auto const& quad : texturedQuads(off))
			require(quad.colour == White,
				"a Facade with its lights off was drawn with a colour other than "
				"its raw artwork");
	}

	//
	// A Window looking into a Facade still shows the Facade's objects and
	// its agents. The Aperture pass tiles the Facade behind the glass and
	// clips to the Window's rect (ADR 0002); what it shows is the same
	// artwork the Solid pass would show.
	//
	void aWindowIntoAFacadeShowsObjectsAndAgents()
	{
		auto const atlas = installFlatSurfaceAtlas();
		pinViewport();

		Frontage f;

		// The glass looks into the Facade.
		auto const back = f.window->getBackSector();
		require(back != nullptr && back->getType() == core::SectorType::Facade,
			"the Window's back Sector is not the Facade");
		require(back->getIndex() == f.facadeIndex, "the Window looks into the wrong Sector");

		// The Aperture pass tiles the Facade with its own artwork, and the
		// glass shows that artwork rather than any colour the Sector carries.
		auto const drawing = renderFlat(f.facade, LayerRenderStyle::Aperture);
		auto const tiles = drawnTiles(drawing, atlas);
		require(countTile(tiles, "facade") >= 2,
			"the aperture pass drew no Facade tile behind the glass");
		for (auto const& quad : texturedQuads(drawing))
			require(quad.colour == White,
				"the Facade behind the glass was drawn with a colour other than "
				"its raw artwork");

		// The glass follows the Facade's geometry: the Window's rect sits
		// wholly inside the Facade, so nothing of the Facade reaches the
		// screen outside the glass.
		core::Vector2 windowMin, windowMax, facadeMin, facadeMax;
		f.window->getFullShape(windowMin, windowMax);
		f.facade->getBounds(facadeMin, facadeMax);
		require(windowMin.x >= facadeMin.x && windowMax.x <= facadeMax.x
				&& windowMin.y >= facadeMin.y && windowMax.y <= facadeMax.y,
			"the Window does not sit wholly inside the Facade it looks into");

		// Objects inside the Facade reach the glass: the aperture pass draws
		// a Sector's objects because it draws solid, and the Facade really
		// hosts one.
		f.world.addSectorMarker(f.facadeIndex, 0, 1.5f, nullptr);
		bool holdsMarker{ false };
		for (uint32_t objectIndex = 0; objectIndex < f.facade->getNumObjects(); ++objectIndex)
		{
			if (f.facade->getObject(objectIndex) != nullptr) holdsMarker = true;
		}
		require(holdsMarker, "the Facade hosts no object to show through the Window");
		require(isDrawnSolid(LayerRenderStyle::Aperture),
			"the aperture pass does not draw objects");

		// Agents inside the Facade reach the glass too, and the wireframe
		// overlay still refuses to show them.
		auto const agentId = f.world.createAgent("Frontage walker", f.facadeIndex, 0, 1.0f);
		require(agentId != core::AgentId{}, "createAgent did not assign the Facade walker an id");
		require(f.facade->getAgents().size() == 1,
			"the Agent did not enter the Facade");
		require(shouldRenderSectorAgents(core::SectorType::Facade, LayerRenderStyle::Aperture),
			"the aperture pass does not render agents inside a Facade");
		require(!shouldRenderSectorAgents(core::SectorType::Facade, LayerRenderStyle::Wireframe),
			"the wireframe overlay renders agents inside a Facade");

		// And with the Facade's lights off, the glass shows exactly the same
		// picture - the light switch moves the agents' world, not the paint.
		std::const_pointer_cast<core::Facade>(f.facade)->lightsOff();
		auto const dark = renderFlat(f.facade, LayerRenderStyle::Aperture);
		require(drawnTiles(dark, atlas) == tiles,
			"the picture behind the glass changed when the Facade's lights went off");
	}

	//
	// A mixed span - a Window looking half into a Facade and half into a
	// Background - is refused at authoring, the same rule #36 set for
	// Background/Location mixes. So the aperture never has to composite a
	// Facade beside a Background, and the single-sector path above is the
	// whole story for Facades behind glass.
	//
	void aWindowCannotLookHalfIntoAFacadeAndHalfIntoABackground()
	{
		core::World world("Mixed span", 12, 2);
		while (world.getLayerCount() < 2) world.addLayer();
		world.addRoom("Front", 0, 0, 0, 12, 1);
		world.addFacade("Frontage", 1, 0, 2, 2, 1);
		world.addBackground(1, 0, 4, 2, 1, core::BackgroundColour{ 10, 10, 10 });

		std::string diagnostic;
		require(!world.canAddSectorWindow(0, 0, 3, 2, 1, &diagnostic),
			"a Window spanning a Facade and a Background was accepted");
		require(diagnostic.find("Background") != std::string::npos,
			("the mixed-span refusal does not name the Background: " + diagnostic).c_str());
	}
}


void render_smoke::registerFacades(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "aFacadeDrawsItsOwnTileAcrossItsArea", isolated<[](smoke::Context const&) { aFacadeDrawsItsOwnTileAcrossItsArea(); }> });
	checks.push_back({ "aBackgroundDrawsItsOwnTileAcrossItsArea", isolated<[](smoke::Context const&) { aBackgroundDrawsItsOwnTileAcrossItsArea(); }> });
	checks.push_back({ "aFacadeIsOutlinedByTheWireframeOverlayAndFillsNothing", isolated<[](smoke::Context const&) { aFacadeIsOutlinedByTheWireframeOverlayAndFillsNothing(); }> });
	checks.push_back({ "aFacadeDrawsTheSamePictureWithItsLightsOff", isolated<[](smoke::Context const&) { aFacadeDrawsTheSamePictureWithItsLightsOff(); }> });
	checks.push_back({ "aWindowIntoAFacadeShowsObjectsAndAgents", isolated<[](smoke::Context const&) { aWindowIntoAFacadeShowsObjectsAndAgents(); }> });
	checks.push_back({ "aWindowCannotLookHalfIntoAFacadeAndHalfIntoABackground", isolated<[](smoke::Context const&) { aWindowCannotLookHalfIntoAFacadeAndHalfIntoABackground(); }> });
}
