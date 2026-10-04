#include "Checks.h"
#include "AgentPoseTestAccess.h"
#include "Render.h"
#include "ObjectTileset.h"
#include "UISettings.h"
#include <cmath>
#include <limits>

extern UISettings gUISettings;
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
		world.attachFurnitureCatalogue("furniture.furniture.yaml", core::FurnitureCatalogue::readFile(
			context.fixture("resources/test-worlds/furniture.furniture.yaml")));
		world.placeFurniture(room, "bed", 3, 0, "Bed");
		world.addSectorMarker(room, 0, 6.5f, "Exit"); world.finishBuild();
		auto id = world.createAgent("Sleeper", room, 0, 0.5f);
		require(world.moveAgentToMarker(id, world.furniture()[0].destinations[0].marker).accepted(), "Bed move refused");
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
				ObjectTileset tiles; tiles.width = 64; tiles.height = 160;
				tiles.sprites.emplace("agent", ObjectSprite{{0, 0, 64, 160}, true});
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
					auto const& a = std::get<WorldDrawList::Triangle>(floor.commands()[i]);
					auto const& b = std::get<WorldDrawList::Triangle>(bed.commands()[i]);
					for (int v = 0; v < 3; ++v)
						require(std::abs(a.positions[v].x - b.positions[v].x - 0.25f * CORE_CELL_WIDTH_PIXELS * zoom) < 0.01f
							&& std::abs(a.positions[v].y - b.positions[v].y - 0.25f * CORE_LEVEL_HEIGHT_PIXELS * zoom) < 0.01f,
							"Bed body must render at Bed x + 0.75 and Floor y + 0.25 in sprite and glyph rendering");
				}
			}
		}
		require(world.moveAgentToMarker(id, world.getMarkerIds().back()).accepted(), "Bed departure refused");
		world.advanceTicks(1800);
		require(sleeper->getPose() == core::Pose::Standing && sleeper->getPoseRenderYOffset() == 0.f
			&& sleeper->getPoseRenderXOffset() == 0.f, "Bed departure retained offset");
		clearObjectTileset(); ImGui::EndFrame();
	}

	void agentPoses(smoke::Context const&)
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
		auto id = world.createAgent("Agent", 0, 0, 3.5f);
		auto* agent = world.lookupAgent(id).entity;
		auto const standingHeight = agent->getHeight(), standingWidth = agent->getWidth();
		auto near = [](float a, float b) { return std::abs(a - b) < 0.01f; };
		for (bool sprite : {false, true})
		{
			clearObjectTileset();
			if (sprite)
			{
				ObjectTileset tiles; tiles.width = 64; tiles.height = 160;
				tiles.sprites.emplace("agent", ObjectSprite{{0, 0, 64, 160}, true});
				setObjectTileset(std::move(tiles), reinterpret_cast<ImTextureID>(1));
			}
			ImVec2 originalMin{}, originalMax{};
			for (auto pose : {core::Pose::Standing, core::Pose::Sitting, core::Pose::Lying})
			{
				core::AgentPoseTestAccess::set(*agent, pose);
				require(world.getSimulationSnapshot().agents.front().pose == pose, "Snapshot did not expose Pose");
				require(near(agent->getHeight(), standingHeight * (pose == core::Pose::Sitting ? 0.6f : 1.f))
					&& near(agent->getWidth(), standingWidth), "Pose changed the wrong physical dimensions");
				core::Vector2 b0, b1; agent->getBounds().getCurrentShape(b0, b1);
				require(near(b1.y - b0.y, agent->getHeight()) && near(b1.x - b0.x, standingWidth), "Pose bounds disagree with physics");
				WorldDrawList list(WorldDrawList::ClipRectangle{{0,0},{800,600}});
				renderAgent(agent, &list);
				if (!sprite && pose == core::Pose::Standing)
					require(list.commands().size() == 1 && std::holds_alternative<WorldDrawList::Text>(list.commands().front()),
						"Standing glyph must retain the unchanged text path");
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
						for (auto p : triangle.positions) { lo.x = std::min(lo.x,p.x); lo.y = std::min(lo.y,p.y); hi.x = std::max(hi.x,p.x); hi.y = std::max(hi.y,p.y); }
					}
					if (pose == core::Pose::Lying)
					{
						auto const& triangle = std::get<WorldDrawList::Triangle>(list.commands().front());
						require(triangle.positions[0].x > triangle.positions[2].x, "Lying head does not point right");
					}
				}
				else
				{
					if (pose != core::Pose::Standing)
					{
						require(list.commands().size() * 3 == static_cast<size_t>(adapter.IdxBuffer.Size), "Posed glyph was not tessellated");
						for (size_t i = 0; i < list.commands().size(); ++i)
						{
							auto const& triangle = std::get<WorldDrawList::Triangle>(list.commands()[i]);
							require(triangle.texture == WorldDrawList::Texture::FontAtlas, "Posed glyph lost its font atlas");
							for (int v = 0; v < 3; ++v)
							{
								auto const& vertex = adapter.VtxBuffer[adapter.IdxBuffer[static_cast<int>(i * 3) + v]];
								require(near(triangle.positions[v].x, vertex.pos.x) && near(triangle.positions[v].y, vertex.pos.y)
									&& near(triangle.texcoords[v].x, vertex.uv.x) && near(triangle.texcoords[v].y, vertex.uv.y),
									"Production glyph stance differs from tessellated glyph geometry");
							}
						}
					}
					for (auto const& vertex : adapter.VtxBuffer) { auto p = vertex.pos; lo.x = std::min(lo.x,p.x); lo.y = std::min(lo.y,p.y); hi.x = std::max(hi.x,p.x); hi.y = std::max(hi.y,p.y); }
				}
				require(hi.x > lo.x && hi.y > lo.y, "Pose produced no visible body");
				if (pose == core::Pose::Standing) { originalMin = lo; originalMax = hi; }
				else if (pose == core::Pose::Sitting)
					require(near(hi.x-lo.x, originalMax.x-originalMin.x) && near(hi.y-lo.y, (originalMax.y-originalMin.y)*0.6f)
						&& near(hi.y, sprite ? originalMax.y : 600.f + (originalMax.y - 600.f) * 0.6f),
						"Sitting must squash only height around the floor anchor (including glyph padding)");
				else require(near(hi.x-lo.x, originalMax.y-originalMin.y) && near(hi.y-lo.y, originalMax.x-originalMin.x)
					&& hi.x-lo.x > standingWidth * CORE_CELL_WIDTH_PIXELS, "Lying must rotate and overflow without squeezing");
			}
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
