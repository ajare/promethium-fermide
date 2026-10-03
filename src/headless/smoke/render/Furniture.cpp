#include "Checks.h"
#include "Render.h"
#include "ObjectTileset.h"
#include "UISettings.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/MarkerSectorObject.h"
#include <fstream>
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
		// Exercise each active route independently: depth 2 must be in front of
		// the desk at depth 2; depth 3 must be behind it. Higher artwork depth
		// renders first even when placement order is the reverse.
		for (bool backRoute : { false, true })
		{
			auto yaml = YAML::LoadFile(context.fixture("resources/test-worlds/desk.furniture.yaml").string());
			yaml["furnitureCatalogue"]["definitions"][0]["edges"].remove(backRoute ? 1 : 4);
			auto filename = context.temporaryRoot() / (backRoute ? "back.furniture.yaml" : "front.furniture.yaml");
			{ std::ofstream file(filename); file << yaml; }
			auto deskWorld = std::make_shared<core::World>("Desk draw ordering", 8, 2);
			auto deskRoom = deskWorld->addRoom("Room", 0, 0, 0, 8, 1);
			deskWorld->attachFurnitureCatalogue(filename.filename().string(), core::FurnitureCatalogue::load(filename));
			deskWorld->placeFurniture(deskRoom, "desk", 2, 0, "Desk", 2);
			uint32_t exitId = 0;
			deskWorld->addSectorMarker(deskRoom, 0, 0.5f, "Entrance");
			deskWorld->addSectorMarker(deskRoom, 0, 6.5f, "Exit", &exitId);
			deskWorld->finishBuild();
			auto agent = deskWorld->lookupAgent(deskWorld->createAgent("Walker", deskRoom, 0, 0.5f)).entity;
			agent->setPath(deskWorld->getGraph()->calculatePath(agent, deskWorld->getGraph()->getVertexByIdentifier(exitId)), true);
			for (int tick = 0; tick < 600 && agent->getGlobalPosition().x < 3; ++tick) deskWorld->advanceTicks(1);
			require(agent->getGlobalPosition().x >= 3 && agent->getGlobalPosition().x < 3.1f
				&& agent->getLocalDepth() == (backRoute ? 3 : 2), "Walker did not enter chosen side route");
			deskWorld->pauseSimulation();
			// Topology replay must not teleport or lose the active visual depth.
			auto position = agent->getGlobalPosition(); auto agentId = deskWorld->getAgentId(agent);
			deskWorld->placeFurniture(deskRoom, "desk", 2.125f, 0, "Deeper artwork", 4);
			deskWorld->finishBuild();
			agent = deskWorld->lookupAgent(agentId).entity;
			require(agent->getGlobalPosition() == position && agent->getLocalDepth() == (backRoute ? 3 : 2),
				"Unrelated Furniture placement changed Agent position/depth");
			RenderWorldScope deskScope(deskWorld);
			for (auto style : { LayerRenderStyle::Solid, LayerRenderStyle::Aperture, LayerRenderStyle::Wireframe, LayerRenderStyle::Hidden })
			{
				WorldDrawList::ClipRectangle clip{{150,450},{240,590}};
				WorldDrawList drawing(clip);
				renderSector(deskWorld->getSector(deskRoom), 0, style, false, ImColor(192,192,255), &drawing);
				size_t deskFirst = drawing.commands().size(), deskLast = 0;
				size_t deepLast = 0, agentFirst = drawing.commands().size(), agentLast = 0;
				unsigned deskTriangles = 0, deepTriangles = 0, agentTriangles = 0;
				for (size_t i = 0; i < drawing.commands().size(); ++i)
					if (auto triangle = std::get_if<WorldDrawList::Triangle>(&drawing.commands()[i]);
						triangle && triangle->texture == WorldDrawList::Texture::ObjectAtlas)
					{
						if (triangle->texcoords[0].x >= 256.f / 320)
						{
							float x = std::min({ triangle->positions[0].x, triangle->positions[1].x, triangle->positions[2].x });
							if (x == 2 * CORE_CELL_WIDTH_PIXELS || x == 3 * CORE_CELL_WIDTH_PIXELS)
							{ ++deskTriangles; deskFirst = std::min(deskFirst, i); deskLast = i; }
							else { ++deepTriangles; deepLast = i; }
						}
						else if (triangle->texcoords[0].x >= 83.f / 320 && triangle->texcoords[0].x < 110.f / 320)
						{ ++agentTriangles; agentFirst = std::min(agentFirst, i); agentLast = i; }
						else continue;
						require(triangle->clip.minimum.x == clip.minimum.x && triangle->clip.maximum.x == clip.maximum.x
							&& triangle->clip.minimum.y == clip.minimum.y && triangle->clip.maximum.y == clip.maximum.y,
							"Depth-ordered content escaped Layer aperture clipping");
					}
				if (style == LayerRenderStyle::Wireframe || style == LayerRenderStyle::Hidden)
					require(!deskTriangles && !deepTriangles && !agentTriangles, "Local depth exposed content on a hidden/wireframe Layer");
				else
				{
					require(deskTriangles == 4 && deepTriangles == 4 && agentTriangles == 2, "Desk/Agent commands missing");
					require(deepLast < deskFirst && deepLast < agentFirst, "Larger Local depth did not render first");
					require(backRoute ? agentLast < deskFirst : deskLast < agentFirst,
						"Active front/back route did not render on the correct side of the desk");
				}
			}
			// Arrive at Furniture, then render the retained depth with no active edge.
			std::shared_ptr<const core::Vertex> seat;
			for (uint32_t i = 0; i < deskWorld->getSector(deskRoom)->getNumObjects(); ++i)
				if (auto marker = std::dynamic_pointer_cast<core::MarkerSectorObject>(deskWorld->getSector(deskRoom)->getObject(i));
					marker && marker->getMarker()->getId() == deskWorld->furniture().front().marker)
					seat = deskWorld->getGraph()->getVertexForObject(marker);
			// Rendering needs a known incoming edge, not an inferred-source
			// approach directly to the nearest seat with no Graph edge to adopt.
			auto seatApproach = seat->getEdges().front()->getOtherVertex(seat);
			agent->setPath(deskWorld->getGraph()->calculatePath(agent, seatApproach, seat), true);
			deskWorld->resumeSimulation();
			for (int tick = 0; tick < 1200 && agent->getState() != core::Agent::State::Idle; ++tick) deskWorld->advanceTicks(1);
			require(agent->getState() == core::Agent::State::Idle && agent->getGlobalPosition() == seat->getPosition()
				&& agent->getLocalDepth() == 2, "Stationary Furniture arrival lost incoming depth");
			for (bool paused : { false, true })
			{
				if (paused) deskWorld->pauseSimulation();
				WorldDrawList drawing({{0,0},{800,600}});
				renderSector(deskWorld->getSector(deskRoom), 0, LayerRenderStyle::Solid, false, ImColor(192,192,255), &drawing);
				size_t deskLast = 0, agentFirst = drawing.commands().size();
				for (size_t i = 0; i < drawing.commands().size(); ++i)
					if (auto triangle = std::get_if<WorldDrawList::Triangle>(&drawing.commands()[i]);
						triangle && triangle->texture == WorldDrawList::Texture::ObjectAtlas)
					{
						if (triangle->texcoords[0].x >= 256.f / 320) deskLast = i;
						else if (triangle->texcoords[0].x >= 83.f / 320 && triangle->texcoords[0].x < 110.f / 320)
							agentFirst = std::min(agentFirst, i);
					}
				require(agentFirst < drawing.commands().size() && deskLast < agentFirst,
					"Stationary/paused Agent rendered behind equal-depth Furniture");
			}
		}
		clearObjectTileset(); ImGui::EndFrame();
	}
}
void render_smoke::registerFurniture(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "furniture/chairCommands", isolated<chairCommands> });
}
