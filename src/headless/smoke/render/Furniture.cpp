#include "Checks.h"
#include "Render.h"
#include "ObjectTileset.h"
#include "UISettings.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/MarkerSectorObject.h"
#include "core/AgentTagRegistryDocument.h"
#include <fstream>
#include <yaml-cpp/yaml.h>

extern UISettings gUISettings;
namespace
{
	void demoCommands(smoke::Context const& context)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize = {1200, 600}; ImGui::GetIO().Fonts->AddFontDefault();
		ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.worldZoom = 1; gUISettings.worldViewportWidth = 1200; gUISettings.worldViewportHeight = 600;
		gUISettings.worldViewportX = 0; gUISettings.worldViewportY = 0; gUISettings.xOffset = 0; gUISettings.yOffset = 0;
		auto manifest = YAML::LoadFile(context.fixture("resources/Resources.yaml").string());
		ObjectTileset tiles; tiles.width = 320; tiles.height = 640;
		bool demoDependency = false, catalogueDependency = false;
		for (auto resource : manifest["Resources"]["Resource"])
		{
			auto name = resource["name"].as<std::string>();
			if (name == "FurnitureDemo") demoDependency = resource["DependentResources"]["DependentResource"]["ref"].as<std::string>() == "FurnitureCatalogue";
			if (name == "FurnitureCatalogue") catalogueDependency = resource["DependentResources"]["DependentResource"]["ref"].as<std::string>() == "ObjectAtlas";
			if (name == "ObjectAtlas")
				for (auto image : resource["Definitions"]["Definition"]["Images"]["Image"])
					tiles.sprites.emplace(image["name"].as<std::string>(), ObjectSprite{{image["x"].as<int>(), image["y"].as<int>(), image["width"].as<int>(), image["height"].as<int>()}, false});
		}
		require(demoDependency && catalogueDependency, "Required bundled Furniture resource dependencies are missing");
		require(std::filesystem::is_regular_file(context.fixture("resources/textures/objects.png")), "Required placeholder atlas is missing");
		auto world = core::loadWorldDocument(context.fixture("resources/test-worlds/furniture.world.yaml"));
		for (auto const& [key, definition] : world->furnitureCatalogue()->definitions())
			for (auto const& tile : definition.tiles)
			{
				auto found = tiles.sprites.find(tile.image);
				require(found != tiles.sprites.end() && found->second.region.width == 64 && found->second.region.height == 160,
					"Required sample artwork must resolve a full World tile");
			}
		setObjectTileset(std::move(tiles), reinterpret_cast<ImTextureID>(1));
		RenderWorldScope scope(world);
		for (auto style : {LayerRenderStyle::Solid, LayerRenderStyle::Aperture, LayerRenderStyle::Wireframe, LayerRenderStyle::Hidden})
		{
			WorldDrawList::ClipRectangle clip{{120,300},{400,590}};
			WorldDrawList drawing(clip);
			renderSector(world->getSector(0), 0, style, false, ImColor(192,192,255), &drawing);
			size_t deskFirst = drawing.commands().size(), deskLast = 0, frontFirst = drawing.commands().size(), backLast = 0;
			unsigned deskCount = 0, sofaCount = 0, frontCount = 0, backCount = 0;
			for (size_t i = 0; i < drawing.commands().size(); ++i)
				if (auto triangle = std::get_if<WorldDrawList::Triangle>(&drawing.commands()[i]);
					triangle && triangle->texture == WorldDrawList::Texture::ObjectAtlas)
				{
					auto uv = triangle->texcoords[0];
					if (uv.y >= 480.f / 640)
					{
						if (uv.x >= 128.f / 320) { ++deskCount; deskFirst = std::min(deskFirst, i); deskLast = i; }
						else ++sofaCount;
					}
					else if (uv.x >= 83.f / 320 && uv.x < 110.f / 320 && uv.y >= 248.f / 640)
					{
						auto x = std::min({triangle->positions[0].x, triangle->positions[1].x, triangle->positions[2].x});
						// Agent sprite's physical half-width is 0.2 World units.
						if (std::abs(x - (2.875f - 0.2f) * CORE_CELL_WIDTH_PIXELS) < 0.01f)
						{
							if (!deskCount) { ++backCount; backLast = i; }
							else { ++frontCount; frontFirst = std::min(frontFirst, i); }
						}
					}
					else continue;
					require(triangle->clip.minimum.x == clip.minimum.x && triangle->clip.maximum.y == clip.maximum.y,
						"Bundled overlapping Furniture escaped Layer/aperture clipping");
				}
			if (style == LayerRenderStyle::Hidden || style == LayerRenderStyle::Wireframe)
				require(!deskCount && !sofaCount && !frontCount && !backCount, "Demo exposed hidden Layer textures");
			else require(deskCount == 4 && sofaCount == 8 && frontCount == 2 && backCount == 2
				&& backLast < deskFirst && deskLast < frontFirst,
				"Bundled demo did not render retained Agents behind/in front of the desk");
		}
		clearObjectTileset(); ImGui::EndFrame();
	}

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
			auto stationary = agent->getGlobalPosition();
			require(deskWorld->editFurniture(deskWorld->furniture().front().id, 4.25f, 0,
				"Moved artwork", &diagnostic, 1), diagnostic);
			agent = deskWorld->lookupAgent(agentId).entity;
			require(agent->getGlobalPosition() == stationary && agent->getLocalDepth() == 2,
				"Moved/depth-edited artwork changed stationary render history");
			WorldDrawList edited({{0,0},{800,600}});
			renderSector(deskWorld->getSector(deskRoom), 0, LayerRenderStyle::Solid, false, ImColor(192,192,255), &edited);
			size_t artworkLast = 0, walkerFirst = edited.commands().size();
			for (size_t i = 0; i < edited.commands().size(); ++i)
				if (auto triangle = std::get_if<WorldDrawList::Triangle>(&edited.commands()[i]);
					triangle && triangle->texture == WorldDrawList::Texture::ObjectAtlas)
				{
					if (triangle->texcoords[0].x >= 256.f / 320) artworkLast = i;
					else if (triangle->texcoords[0].x >= 83.f / 320 && triangle->texcoords[0].x < 110.f / 320)
						walkerFirst = std::min(walkerFirst, i);
				}
			require(walkerFirst < artworkLast, "Edited artwork did not render in front of retained deeper Agent");
			require(deskWorld->removeFurniture(deskWorld->furniture().front().id, &diagnostic), diagnostic);
			agent = deskWorld->lookupAgent(agentId).entity;
			require(agent->getGlobalPosition() == stationary && agent->getLocalDepth() == 2,
				"Furniture deletion changed stationary rendering history");
		}
		// A chair attached in the middle of the desk's front route renders using
		// the actual traversal depth, without relying on equal-cost route choice.
		auto attached = std::make_shared<core::World>("Attached rendering", 8, 2);
		auto attachedRoom = attached->addRoom("Room", 0, 0, 0, 8, 1);
		attached->attachFurnitureCatalogue("attachments.furniture.yaml",
			core::FurnitureCatalogue::load(context.fixture("resources/test-worlds/attachments.furniture.yaml")));
		attached->placeFurniture(attachedRoom, "desk", 2.125f, 0, "Desk", 2);
		auto attachedChair = attached->placeFurniture(attachedRoom, "chair", 3.25f, 0, "Chair", 1);
		auto attachedMarker = attached->furniture().back().marker;
		attached->finishBuild();
		std::shared_ptr<const core::Vertex> attachedSeat;
		for (uint32_t i = 0; i < attached->getSector(attachedRoom)->getNumObjects(); ++i)
			if (auto marker = std::dynamic_pointer_cast<core::MarkerSectorObject>(attached->getSector(attachedRoom)->getObject(i));
				marker && marker->getMarker()->getId() == attachedMarker)
				attachedSeat = attached->getGraph()->getVertexForObject(marker);
		auto visitorId = attached->createAgent("Visitor", attachedRoom, 0, 0.5f);
		auto visitor = attached->lookupAgent(visitorId).entity;
		visitor->setPath(attached->getGraph()->calculatePath(visitor, attachedSeat), true);
		for (int tick = 0; tick < 900 && visitor->getGlobalPosition().x < 3.375f; ++tick) attached->advanceTicks(1);
		require(visitor->getGlobalPosition().x >= 3.375f && visitor->getGlobalPosition().x < 3.75f
			&& visitor->getLocalDepth() == 2, "Attached visitor did not traverse the matching-depth branch");
		RenderWorldScope attachedScope(attached);
		auto checkAttached = [&] {
			for (auto style : { LayerRenderStyle::Solid, LayerRenderStyle::Aperture, LayerRenderStyle::Wireframe, LayerRenderStyle::Hidden })
			{
				WorldDrawList::ClipRectangle clip{{150,450},{270,590}};
				WorldDrawList drawing(clip);
				renderSector(attached->getSector(attachedRoom), 0, style, false, ImColor(192,192,255), &drawing);
				size_t deskLast = 0, agentFirst = drawing.commands().size(), agentLast = 0, chairFirst = drawing.commands().size();
				unsigned deskTriangles = 0, chairTriangles = 0, agentTriangles = 0;
				for (size_t i = 0; i < drawing.commands().size(); ++i)
					if (auto triangle = std::get_if<WorldDrawList::Triangle>(&drawing.commands()[i]);
						triangle && triangle->texture == WorldDrawList::Texture::ObjectAtlas)
					{
						if (triangle->texcoords[0].x >= 256.f / 320)
						{
							auto x = std::min({triangle->positions[0].x, triangle->positions[1].x, triangle->positions[2].x});
							if (x == attached->furniture().back().x * CORE_CELL_WIDTH_PIXELS)
							{ ++chairTriangles; chairFirst = std::min(chairFirst, i); }
							else { ++deskTriangles; deskLast = i; }
						}
						else if (triangle->texcoords[0].x >= 83.f / 320 && triangle->texcoords[0].x < 110.f / 320)
						{ ++agentTriangles; agentFirst = std::min(agentFirst, i); agentLast = i; }
						else continue;
						require(triangle->clip.minimum.x == clip.minimum.x && triangle->clip.minimum.y == clip.minimum.y
							&& triangle->clip.maximum.x == clip.maximum.x && triangle->clip.maximum.y == clip.maximum.y,
							"Attached content escaped aperture clipping");
					}
				if (style == LayerRenderStyle::Wireframe || style == LayerRenderStyle::Hidden)
					require(!deskTriangles && !chairTriangles && !agentTriangles, "Attachment exposed hidden Layer content");
				else
					require(deskTriangles == 4 && chairTriangles == 2 && agentTriangles == 2
						&& deskLast < agentFirst && agentLast < chairFirst,
						"Attached commands lost fractional artwork or resolved-depth order");
			}
		};
		checkAttached();
		attached->advanceTicks(900);
		require(visitor->getGlobalPosition().x == 3.75f && visitor->getLocalDepth() == 2,
			"Attached stationary destination lost incoming depth");
		checkAttached(); attached->pauseSimulation();
		auto position = visitor->getGlobalPosition();
		require(attached->editFurniture(attachedChair, 2.375f, 0, "Detached", &diagnostic), diagnostic);
		visitor = attached->lookupAgent(visitorId).entity;
		require(visitor->getGlobalPosition() == position && visitor->getLocalDepth() == 2,
			"Detached rendering rebuild changed Agent physical/depth history");
		checkAttached();
		clearObjectTileset(); ImGui::EndFrame();
	}
}
void render_smoke::registerFurniture(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "furniture/chairCommands", isolated<chairCommands> });
	checks.push_back({ "furniture/demoCommands", isolated<demoCommands> });
}
