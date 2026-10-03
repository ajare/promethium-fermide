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
		world->placeFurniture(room, "chair", 2.25f, 0, "Chair"); world->finishBuild();
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
						require((point.x == 2.25f * CORE_CELL_WIDTH_PIXELS || point.x == 3.25f * CORE_CELL_WIDTH_PIXELS)
							&& (point.y == 600 || point.y == 600 - CORE_LEVEL_HEIGHT_PIXELS), "Chair artwork was scaled or lost fractional placement");
				}
			require(triangles == expected, "Chair draw-command/Layer style mismatch");
		};
		check(LayerRenderStyle::Solid, {{0,0},{800,600}}, 2);
		check(LayerRenderStyle::Aperture, {{150,450},{190,590}}, 2);
		check(LayerRenderStyle::Wireframe, {{0,0},{800,600}}, 0);
		clearObjectTileset(); ImGui::EndFrame();
	}
}
void render_smoke::registerFurniture(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "furniture/chairCommands", isolated<chairCommands> });
}
