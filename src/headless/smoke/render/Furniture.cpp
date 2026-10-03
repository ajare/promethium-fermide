#include "Checks.h"
#include "Render.h"
#include "ObjectTileset.h"
#include "UISettings.h"
#include "core/World.h"
#include <yaml-cpp/yaml.h>

extern UISettings gUISettings;
namespace
{
	void chairCommands(smoke::Context const& context)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize = {800, 600}; ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.worldZoom = 1; gUISettings.worldViewportWidth = 800; gUISettings.worldViewportHeight = 600;
		gUISettings.worldViewportX = 0; gUISettings.worldViewportY = 0; gUISettings.xOffset = 0; gUISettings.yOffset = 0;
		// Resolve the same Image-set rectangle as the rendering service, rather
		// than manufacturing a second Furniture/TileSet metadata authority.
		auto manifest = YAML::LoadFile(context.fixture("resources/Resources.yaml").string());
		ObjectTileset tiles; tiles.width = 320; tiles.height = 480;
		for (auto resource : manifest["Resources"]["Resource"])
			if (resource["name"].as<std::string>() == "ObjectAtlas")
				for (auto image : resource["Definitions"]["Definition"]["Images"]["Image"])
						tiles.sprites.emplace(image["name"].as<std::string>(), ObjectSprite{{image["x"].as<int>(), image["y"].as<int>(), image["width"].as<int>(), image["height"].as<int>()}, false});
		require(tiles.sprites.contains("chair"), "Required chair Image-set artwork is missing");
		setObjectTileset(std::move(tiles), reinterpret_cast<ImTextureID>(1));
		auto world = std::make_shared<core::World>("Chair rendering", 8, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 8, 1);
		world->attachFurnitureCatalogue("chair.furniture.yaml", core::FurnitureCatalogue::load(context.fixture("resources/test-worlds/chair.furniture.yaml")));
		auto id = world->placeFurniture(room, "chair", 2.25f, 0, "Chair"); world->finishBuild();
		float artworkX = 2.25f;
		RenderWorldScope scope(world);
		auto check = [&](LayerRenderStyle style, WorldDrawList::ClipRectangle clip, unsigned expected) {
			WorldDrawList drawing(clip);
			renderSector(world->getSector(room), 0, style, false, ImColor(192,192,255), &drawing);
			unsigned triangles = 0;
			for (auto const& command : drawing.commands())
				if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
					triangle && triangle->texture == WorldDrawList::Texture::ObjectAtlas
					&& triangle->texcoords[0].x >= 256.0f / 320 && triangle->texcoords[0].y <= 320.0f / 480)
				{
					++triangles;
					require(triangle->clip.minimum.x == clip.minimum.x && triangle->clip.maximum.x == clip.maximum.x, "Chair escaped aperture clipping");
					for (auto point : triangle->positions)
						require((point.x == artworkX * CORE_CELL_WIDTH_PIXELS || point.x == (artworkX + 1) * CORE_CELL_WIDTH_PIXELS)
							&& (point.y == 600 || point.y == 600 - CORE_LEVEL_HEIGHT_PIXELS), "Chair artwork was scaled or lost fractional placement");
				}
			require(triangles == expected, "Chair draw-command/Layer style mismatch");
		};
		check(LayerRenderStyle::Solid, {{0,0},{800,600}}, 2);
		check(LayerRenderStyle::Aperture, {{150,450},{190,590}}, 2);
		check(LayerRenderStyle::Wireframe, {{0,0},{800,600}}, 0);
		world->pauseSimulation();
		std::string diagnostic;
		require(world->editFurniture(id, 4.25f, 0, "Moved chair", &diagnostic), "Could not move rendered Furniture");
		artworkX = 4.25f;
		check(LayerRenderStyle::Solid, {{0,0},{800,600}}, 2);
		require(world->removeFurniture(id, &diagnostic), "Could not delete rendered Furniture");
		check(LayerRenderStyle::Solid, {{0,0},{800,600}}, 0);
		auto layouts = std::make_shared<core::World>("Layout rendering", 20, 4);
		auto layoutRoom = layouts->addRoom("Room", 0, 0, 0, 20, 4);
		layouts->attachFurnitureCatalogue("layouts.furniture.yaml", core::FurnitureCatalogue::load(context.fixture("resources/test-worlds/layouts.furniture.yaml")));
		auto sofa = layouts->placeFurniture(layoutRoom, "sofa", 1.125f, 0, "Sofa");
		auto larger = layouts->placeFurniture(layoutRoom, "larger", 8.375f, 0, "Larger");
		layouts->finishBuild(); layouts->pauseSimulation();
		RenderWorldScope layoutScope(layouts);
		auto checkLayouts = [&](LayerRenderStyle style, WorldDrawList::ClipRectangle clip) {
			WorldDrawList drawing(clip);
			renderSector(layouts->getSector(layoutRoom), 0, style, false, ImColor(192,192,255), &drawing);
			std::map<std::pair<float, float>, unsigned> tilesDrawn;
			for (auto const& command : drawing.commands())
				if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
					triangle && triangle->texture == WorldDrawList::Texture::ObjectAtlas
					&& triangle->texcoords[0].x >= 256.0f / 320 && triangle->texcoords[0].y <= 320.0f / 480)
				{
					require(triangle->clip.minimum.x == clip.minimum.x && triangle->clip.maximum.x == clip.maximum.x
						&& triangle->clip.minimum.y == clip.minimum.y && triangle->clip.maximum.y == clip.maximum.y,
						"Layout draw command escaped clipping");
					float minX = triangle->positions[0].x, maxX = minX;
					float minY = triangle->positions[0].y, maxY = minY;
					for (auto p : triangle->positions) { minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
						minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); }
					require(maxX - minX == CORE_CELL_WIDTH_PIXELS && maxY - minY == CORE_LEVEL_HEIGHT_PIXELS,
						"Multi-tile artwork was arbitrarily scaled");
					++tilesDrawn[{minX, maxY}];
				}
			if (style == LayerRenderStyle::Wireframe) require(tilesDrawn.empty(), "Wireframe rendered tile textures");
			else
			{
				size_t total = 0;
				for (auto const& instance : layouts->furniture())
					for (auto const& tile : layouts->furnitureCatalogue()->definition(instance.definitionKey)->tiles)
					{
						++total;
						require(tilesDrawn[{(instance.x + tile.x) * CORE_CELL_WIDTH_PIXELS,
							600 - (instance.y + tile.y) * CORE_LEVEL_HEIGHT_PIXELS}] == 2,
							"Tile command lost fractional origin or rigid integer offset");
					}
				require(tilesDrawn.size() == total, "Renderer filled transparent layout gaps with extra tiles");
			}
		};
		checkLayouts(LayerRenderStyle::Solid, {{0,0},{800,600}});
		checkLayouts(LayerRenderStyle::Aperture, {{100,400},{500,590}});
		checkLayouts(LayerRenderStyle::Wireframe, {{0,0},{800,600}});
		require(layouts->editFurniture(sofa, 3.625f, 0, "Moved sofa", &diagnostic), diagnostic);
		require(layouts->editFurniture(larger, 11.875f, 0, "Moved larger", &diagnostic), diagnostic);
		checkLayouts(LayerRenderStyle::Solid, {{0,0},{800,600}});
		clearObjectTileset(); ImGui::EndFrame();
	}
}
void render_smoke::registerFurniture(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "furniture/chairCommands", isolated<chairCommands> });
}
