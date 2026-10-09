#include "core/AgentType.h"
#include "Checks.h"
#include "AgentPoseTestAccess.h"
#include "Render.h"
#include "ObjectTileset.h"
#include "UISettings.h"
#include <cmath>
#include <cfloat>
#include <limits>
#include <fstream>
#include <iterator>

extern UISettings gUISettings;
extern core::Agent* gSelectedAgent;
namespace
{
	void bedRenderOffset(smoke::Context const& context)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize = {800, 600};
		ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build();
		ImGui::NewFrame();
		gUISettings.worldViewportWidth = 800; gUISettings.worldViewportHeight = 600;
		gUISettings.worldViewportX = 0; gUISettings.worldViewportY = 0;
		gUISettings.xOffset = 0; gUISettings.yOffset = 0;
		gUISettings.renderAgentDebug = false;
		core::World world("Bed offset", 8, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
		world.attachFurnitureCatalogue("furniture.furniture.lua", core::FurnitureCatalogue::readFile(
			context.fixture("src/headless/smoke/fixtures/furniture/furniture.furniture.lua")));
		world.placeFurniture(room, "bed", 3, 0, "Bed");
		world.addSectorMarker(room, 0, 6.5f, "Exit"); world.finishBuild();
		auto id = world.createAgent("Sleeper", room, 0, 0.5f);
		require(world.moveAgentToMarker(id, world.furniture()[0].destinations[0].marker, core::UseFurnitureAction).accepted(), "Bed move refused");
		world.advanceTicks(1800);
		auto* sleeper = world.lookupAgent(id).entity;
		require(sleeper->getPose() == core::Pose::Lying && sleeper->getPoseRenderYOffset() == 0.25f
			&& sleeper->getGlobalPosition().x + sleeper->getPoseRenderXOffset() == 3.75f,
			"Occupied Bed did not set mattress offset");
		auto position = sleeper->getGlobalPosition();
		auto referenceId = world.createAgent("Floor reference", room, 0, 4.f);
		auto* reference = world.lookupAgent(referenceId).entity;
		core::AgentPoseTestAccess::set(*reference, core::Pose::Lying);
		require(reference->getGlobalPosition() == position && reference->getPoseRenderYOffset() == 0.f
			&& reference->getPoseRenderXOffset() == 0.f,
			"Bed offset changed physical position or free Lying pose");
		for (bool sprite : {false, true})
		{
			clearObjectTileset();
			if (sprite)
			{
				auto tiles = ObjectTileset::load(context.fixture("resources/textures/objects.tileset.yaml"));
				setObjectTileset(std::move(tiles), reinterpret_cast<ImTextureID>(1));
			}
			for (float zoom : {1.f, 2.f})
			{
				gUISettings.worldZoom = zoom;
				WorldDrawList floor(WorldDrawList::ClipRectangle{{-2000,-2000},{2000,2000}});
				WorldDrawList bed(WorldDrawList::ClipRectangle{{-2000,-2000},{2000,2000}});
				renderAgent(reference, &floor); renderAgent(sleeper, &bed);
				require(!floor.commands().empty() && floor.commands().size() == bed.commands().size(), "Bed offset changed body geometry");
				for (size_t i = 0; i < floor.commands().size(); ++i)
				{
					auto checkOffset = [&](ImVec2 a, ImVec2 b) {
						require(std::abs(a.x - b.x - 0.25f * CORE_CELL_WIDTH_PIXELS * zoom) < 0.01f
							&& std::abs(a.y - b.y - 0.25f * CORE_LEVEL_HEIGHT_PIXELS * zoom) < 0.01f,
							"Bed body must retain artwork offsets in sprite and glyph rendering");
					};
					if (auto text = std::get_if<WorldDrawList::Text>(&floor.commands()[i]))
						checkOffset(text->position, std::get<WorldDrawList::Text>(bed.commands()[i]).position);
					else
					{
						auto const& a = std::get<WorldDrawList::Triangle>(floor.commands()[i]);
						auto const& b = std::get<WorldDrawList::Triangle>(bed.commands()[i]);
						for (int v = 0; v < 3; ++v) checkOffset(a.positions[v], b.positions[v]);
					}
				}
			}
		}
		require(world.moveAgentToMarker(id, world.getMarkerIds().back()).accepted(), "Bed departure refused");
		world.advanceTicks(1800);
		require(sleeper->getPose() == core::Pose::Standing && sleeper->getPoseRenderYOffset() == 0.f
			&& sleeper->getPoseRenderXOffset() == 0.f, "Bed departure retained offset");
		clearObjectTileset(); ImGui::EndFrame();
	}

	void agentPoses(smoke::Context const& context)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize = {800, 600};
		ImGui::GetIO().Fonts->AddFontDefault();
		ImGui::GetIO().Fonts->Build();
		ImGui::NewFrame();
		gUISettings.worldZoom = 1;
		gUISettings.worldViewportWidth = 800; gUISettings.worldViewportHeight = 600;
		gUISettings.worldViewportX = 0; gUISettings.worldViewportY = 0;
		gUISettings.xOffset = 0; gUISettings.yOffset = 0;
		gUISettings.renderAgentDebug = false;
		core::World world("Poses", 8, 2);
		world.addRoom("Room", 0, 0, 0, 8, 1); world.finishBuild();
		auto definition = core::bundledHumanAgentType();
		auto replace = [&](std::string const& from, std::string const& to) {
			auto at = definition.source.find(from);
			require(at != std::string::npos, "Pose tile fixture source field missing");
			definition.source.replace(at, from.size(), to);
		};
		replace("type_id = \"Human\"", "type_id = \"PoseTiles\"");
		for (auto name : {"standing", "sitting", "lying", "crouching", "crawling"})
			replace(std::string("image_tile = \"human-") + name + "\"", std::string("image_tile = \"pose-") + name + "\"");
		std::string diagnostic;
		require(world.attachAgentType("pose-tiles.agent.lua", definition.source, &diagnostic), diagnostic);
		auto id = world.createAgent("PoseTiles", "Agent", 0, 0, 3.5f);
		auto* agent = world.lookupAgent(id).entity;
		auto const standingHeight = agent->getHeight(), standingWidth = agent->getWidth();
		auto near = [](float a, float b) { return std::abs(a - b) < 0.01f; };
		for (bool sprite : {false, true})
		{
			clearObjectTileset();
			if (sprite)
			{
				ObjectTileset tiles; tiles.width = 384; tiles.height = 160;
				int index = 0;
				for (auto name : {"standing", "sitting", "lying", "crouching", "crawling"})
					tiles.sprites.emplace(std::string("pose-") + name, ObjectSprite{{64 * index++, 0, 64, 160}, true});
				// A generic tile must not override a pose's declared artwork.
				tiles.sprites.emplace("agent", ObjectSprite{{320, 0, 64, 160}, true});
				setObjectTileset(std::move(tiles), reinterpret_cast<ImTextureID>(1));
			}
			int tileIndex = 0;
			ImVec2 originalMin{}, originalMax{};
			for (auto pose : {core::Pose::Standing, core::Pose::Sitting, core::Pose::Lying,
				core::Pose::Crouching, core::Pose::Crawling})
			{
				core::AgentPoseTestAccess::set(*agent, pose);
				auto const heightScale = agent->getPoseHeightScale();
				require(world.getSimulationSnapshot().agents.front().pose == pose, "Snapshot did not expose Pose");
				require(near(agent->getHeight(), standingHeight * heightScale)
					&& near(agent->getWidth(), standingWidth * agent->getPhysicalBaseline().poses.at(pose).widthRatio),
					"Pose did not apply the declared physical dimension ratios");
				core::Vector2 b0, b1; agent->getBounds().getCurrentShape(b0, b1);
				require(near(b1.y - b0.y, agent->getHeight()) && near(b1.x - b0.x, agent->getWidth()), "Pose bounds disagree with physics");
				WorldDrawList list(WorldDrawList::ClipRectangle{{0,0},{800,600}});
				renderAgent(agent, &list);
				if (!sprite)
					require(list.commands().size() == 1 && std::holds_alternative<WorldDrawList::Text>(list.commands().front()),
						"Missing artwork must use an untransformed glyph fallback");
				ImDrawList adapter(ImGui::GetDrawListSharedData());
				adapter._ResetForNewFrame();
				adapter.PushTextureID(ImGui::GetIO().Fonts->TexID);
				adapter.PushClipRect({-1000,-1000},{2000,2000});
				WorldDrawList adapted(&adapter); renderAgent(agent, &adapted);
				ImVec2 lo{std::numeric_limits<float>::max(),std::numeric_limits<float>::max()}, hi{-lo.x,-lo.y};
				if (sprite)
				{
					require(list.commands().size() == 2, "Sprite pose must retain its two textured triangles");
					for (auto const& command : list.commands())
					{
						auto const& triangle = std::get<WorldDrawList::Triangle>(command);
						require(triangle.texture == WorldDrawList::Texture::ObjectAtlas, "Pose lost sprite texture");
						for (auto uv : triangle.texcoords)
							require(uv.x >= tileIndex / 6.f - .00001f && uv.x <= (tileIndex + 1) / 6.f + .00001f,
								"Renderer ignored the script-defined pose image tile");
						for (auto p : triangle.positions) { lo.x = std::min(lo.x,p.x); lo.y = std::min(lo.y,p.y); hi.x = std::max(hi.x,p.x); hi.y = std::max(hi.y,p.y); }
					}
					auto const& triangle = std::get<WorldDrawList::Triangle>(list.commands().front());
					require(triangle.positions[0].x <= triangle.positions[2].x
						&& triangle.positions[0].y <= triangle.positions[2].y,
						"Renderer rotated declared artwork");
				}
				else
				{
					for (auto const& vertex : adapter.VtxBuffer) { auto p = vertex.pos; lo.x = std::min(lo.x,p.x); lo.y = std::min(lo.y,p.y); hi.x = std::max(hi.x,p.x); hi.y = std::max(hi.y,p.y); }
				}
				require(hi.x > lo.x && hi.y > lo.y, "Pose produced no visible body");
				if (pose == core::Pose::Standing) { originalMin = lo; originalMax = hi; }
				else require(near(hi.x-lo.x, originalMax.x-originalMin.x)
					&& near(hi.y-lo.y, originalMax.y-originalMin.y) && near(hi.y, originalMax.y),
					"Equal-size declared tiles must not be rotated or squashed by Pose");
				++tileIndex;
			}
			// Standing Human appearance retains the historical uniform Height
			// scaling in both sprite and glyph paths, despite unchanged physical width.
			world.pauseSimulation();
			core::AgentPoseTestAccess::set(*agent, core::Pose::Standing);
			auto extent = [&]()
			{
				ImDrawList adapter(ImGui::GetDrawListSharedData());
				adapter._ResetForNewFrame();
				adapter.PushTextureID(ImGui::GetIO().Fonts->TexID);
				adapter.PushClipRect({-1000,-1000},{2000,2000});
				WorldDrawList list(&adapter); renderAgent(agent, &list);
				ImVec2 lo{FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX};
				for (auto const& vertex : adapter.VtxBuffer)
				{
					lo.x = std::min(lo.x, vertex.pos.x); lo.y = std::min(lo.y, vertex.pos.y);
					hi.x = std::max(hi.x, vertex.pos.x); hi.y = std::max(hi.y, vertex.pos.y);
				}
				return ImVec2{hi.x-lo.x, hi.y-lo.y};
			};
			auto const original = extent();
			for (float modifier : {0.7f, 0.9f})
			{
				require(world.setAgentIndividualHeightModifier(id, modifier), "Human Height authoring refused");
				auto const dimensions = core::Agent::placementDimensions(core::bundledHumanBaseline(), modifier);
				require(near(agent->getBounds().getSize().x, dimensions.x)
					&& near(agent->getBounds().getSize().y, dimensions.y),
					"Human render bounds disagreed with modified preview dimensions");
				auto const modified = extent();
				require(near(modified.x, original.x * modifier) && near(modified.y, original.y * modifier),
					"Human Height changed sprite/glyph appearance scaling");
			}
			require(world.setAgentIndividualHeightModifier(id, {}), "Human Height reset refused");
		}
		// Real Human tiles carry their shortened/rotated artwork already. The
		// renderer uses native tile dimensions and unchanged UV orientation.
		auto tiles = ObjectTileset::load(context.fixture("resources/textures/objects.tileset.yaml"));
		auto const regions = tiles.sprites;
		auto const atlasWidth = tiles.width, atlasHeight = tiles.height;
		setObjectTileset(std::move(tiles), reinterpret_cast<ImTextureID>(1));
		auto humanId = world.createAgent("Human reference", 0, 0, 4.5f);
		auto* human = world.lookupAgent(humanId).entity;
		ImVec2 standingSize{};
		for (auto pose : {core::Pose::Standing, core::Pose::Sitting, core::Pose::Lying,
			core::Pose::Crouching, core::Pose::Crawling})
		{
			core::AgentPoseTestAccess::set(*human, pose);
			auto const& region = regions.at(human->getPoseImageTile()).region;
			WorldDrawList list(WorldDrawList::ClipRectangle{{-2000,-2000},{2000,2000}});
			renderAgent(human, &list);
			require(list.commands().size() == 2, "Human did not draw its dedicated pose tile");
			auto const& first = std::get<WorldDrawList::Triangle>(list.commands().front());
			require(near(first.texcoords[0].x, (region.x + .5f) / atlasWidth)
				&& near(first.texcoords[0].y, (region.y + .5f) / atlasHeight),
				"Human artwork was rotated or mirrored at runtime");
			ImVec2 lo{FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX};
			for (auto const& command : list.commands())
				for (auto point : std::get<WorldDrawList::Triangle>(command).positions)
				{
					lo.x = std::min(lo.x, point.x); lo.y = std::min(lo.y, point.y);
					hi.x = std::max(hi.x, point.x); hi.y = std::max(hi.y, point.y);
				}
			if (pose == core::Pose::Standing) standingSize = {hi.x - lo.x, hi.y - lo.y};
			require(near(hi.x - lo.x, standingSize.x * region.width / 26.f)
				&& near(hi.y - lo.y, standingSize.y * region.height / 72.f),
				"Human baked artwork was squashed or rotated again");
		}
		// A short non-Human Standing tile keeps its own proportions and UVs.
		std::ifstream botInput(context.fixture("resources/test-worlds/cleaning-bot.agent.lua"));
		std::string botSource{std::istreambuf_iterator<char>(botInput), {}};
		require(world.attachAgentType("cleaning-bot.agent.lua", botSource, &diagnostic), diagnostic);
		auto botId = world.createAgent("CleaningBot", "Cleaner", 0, 0, 5.5f);
		auto const& botRegion = regions.at("cleaning-bot-standing").region;
		auto* bot = world.lookupAgent(botId).entity;
		struct SelectionScope {
			core::Agent* previous{gSelectedAgent};
			~SelectionScope() { gSelectedAgent = previous; }
		} selectionScope;
		gSelectedAgent = bot;
		for (float zoom : {1.f, 2.f})
		{
			gUISettings.worldZoom = zoom;
			WorldDrawList list(WorldDrawList::ClipRectangle{{-2000,-2000},{2000,2000}});
			renderAgent(world.lookupAgent(botId).entity, &list);
			require(list.commands().size() == 2, "CleaningBot did not draw its dedicated tile");
			auto const& triangle = std::get<WorldDrawList::Triangle>(list.commands().front());
			require(near(triangle.texcoords[0].x, (botRegion.x + .5f) / atlasWidth)
				&& near(triangle.texcoords[0].y, (botRegion.y + .5f) / atlasHeight),
				"CleaningBot drew another Agent's artwork");
			ImVec2 lo{FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX};
			for (auto const& command : list.commands())
			{
				auto const& body = std::get<WorldDrawList::Triangle>(command);
				require(body.colour == IM_COL32(251, 188, 4, 255),
					"Selected CleaningBot tile did not receive the yellow selection tint");
				for (auto point : body.positions)
				{
					lo.x = std::min(lo.x, point.x); lo.y = std::min(lo.y, point.y);
					hi.x = std::max(hi.x, point.x); hi.y = std::max(hi.y, point.y);
				}
			}
			require(near((hi.x - lo.x) / (hi.y - lo.y), 26.f / 24.f)
				&& hi.x - lo.x <= .4f * CORE_CELL_WIDTH_PIXELS * zoom + .01f
				&& hi.y - lo.y <= .15f * CORE_LEVEL_HEIGHT_PIXELS * zoom + .01f,
				"CleaningBot artwork inherited Human proportions or exceeded its baseline");
		}
		clearObjectTileset(); ImGui::EndFrame();
	}
}
namespace render_smoke
{
	void registerAgentPoses(std::vector<smoke::Check>& checks)
	{
		checks.push_back({"agentPoses", isolated<agentPoses>});
		checks.push_back({"furniture/bedRenderOffset", isolated<bedRenderOffset>});
	}
}
