#include "Checks.h"
// Rendering half of the two-sided Door Button checks (#125, #195).
// Core authoring, persistence and traversal checks remain in
// world/TwoSidedButtons.cpp; this module owns only visible geometry.
//   * the back-side Button renders as an outline only - including when its
//     own Layer is the one drawn solid - while the front-side Button fills
//   * visibility is judged from the WorldDrawList command stream under its
//     own clips: a closed Door's zero-area aperture shows no far-side Button,
//     while a visibly open aperture still must (#195)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "imgui/imgui.h"

#include "ObjectTileset.h"
#include "Render.h"
#include "UISettings.h"

#include "core/World.h"
#include "core/Button.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct DoorLocation
	{
		uint32_t sectorIndex{ ~0u };
		uint32_t objectIndex{ ~0u };
		std::shared_ptr<const core::DoorSectorObject> object;
	};

	// The ordinary Door in a freshly built two-Room scene, wherever it
	// registered itself.
	DoorLocation findDoor(core::World const& world)
	{
		for (uint32_t s = 0; s < world.getNumSectors(); ++s)
		{
			auto sector = world.getSector(s);
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
				return { s, i, std::static_pointer_cast<const core::DoorSectorObject>(object) };
			}
		}
		return {};
	}

	// Two Rooms on adjacent Layers with one ordinary Door between them.
	// doorOptions, when given, are applied to the Door at creation.
	struct Scene
	{
		std::unique_ptr<core::World> world;
		DoorLocation door;
		uint32_t frontSector{ ~0u };
		uint32_t backSector{ ~0u };

		core::World& operator*() const { return *world; }
		core::World* operator->() const { return world.get(); }
	};

	Scene buildTwoRoomScene(char const* name, uint32_t foreWidth = 8, uint32_t foreX = 0,
		core::World::CreateDoorOptions const* doorOptions = nullptr, uint32_t doorX = ~0u)
	{
		Scene scene;
		scene.world = std::make_unique<core::World>(name, 16, 2);
		while (scene.world->getLayerCount() < 2) scene.world->addLayer();
		scene.world->addRoom("Fore", 0, 0, foreX, foreWidth, 1);
		scene.world->addRoom("Aft", 1, 0, 0, 16, 1);
		if (doorX == ~0u) doorX = foreX + 2;
		auto const created = doorOptions
			? scene.world->addSectorDoor(0, 0, doorX, *doorOptions)
			: scene.world->addSectorDoor(0, 0, doorX, {});
		scene.world->finishBuild();
		scene.frontSector = created.door.sector->getIndex();
		scene.door = findDoor(*scene.world);
		require(scene.door.object != nullptr, "The scene's Door could not be found");
		scene.backSector = scene.door.object->getDoor()->getBackSector()->getIndex();
		return scene;
	}

	ImU32 const kEnabledButtonColour = ImU32(ImColor(0, 255, 128));
	// BackLocationColour: the tint the renderer gives the Sector revealed through
	// a Door's aperture.
	ImU32 const kBackLayerFillColour = ImU32(ImColor(224, 224, 255));

	// The viewport the headless render checks draw into. The recording
	// WorldDrawList carries it as the root clip, so every nested aperture clip is
	// intersected against it exactly as the production MPP path does.
	WorldDrawList::ClipRectangle const kViewportClip{
		{ 0.0f, 0.0f }, { 1280.0f, 720.0f } };

	struct ScreenBounds
	{
		float minX, maxX, minY, maxY;
	};

	ScreenBounds boundsOf(ImVec2 const* points, int count, float padding)
	{
		ScreenBounds bounds{ points[0].x, points[0].x, points[0].y, points[0].y };
		for (int i = 1; i < count; ++i)
		{
			bounds.minX = std::min(bounds.minX, points[i].x);
			bounds.maxX = std::max(bounds.maxX, points[i].x);
			bounds.minY = std::min(bounds.minY, points[i].y);
			bounds.maxY = std::max(bounds.maxY, points[i].y);
		}
		bounds.minX -= padding;
		bounds.maxX += padding;
		bounds.minY -= padding;
		bounds.maxY += padding;
		return bounds;
	}

	// Whether any of a command's geometry survives its own clip rectangle. The
	// production MPP path discards a zero-area clip outright, so a check that
	// merely tallied recorded commands would overstate what the viewport shows.
	bool survivesClip(ScreenBounds const& bounds, WorldDrawList::ClipRectangle const& clip)
	{
		auto const visibleWidth = std::min(bounds.maxX, clip.maximum.x)
			- std::max(bounds.minX, clip.minimum.x);
		auto const visibleHeight = std::min(bounds.maxY, clip.maximum.y)
			- std::max(bounds.minY, clip.minimum.y);
		return visibleWidth > 0.0f && visibleHeight > 0.0f;
	}

	// The visible geometry the render checks care about, split by how it was
	// painted. Every command is filtered through its own clip first, so a
	// zero-area aperture contributes nothing - exactly as the production MPP
	// path skips such clips.
	struct VisibleGeometry
	{
		int buttonFillTriangles{ 0 };
		int buttonOutlineLines{ 0 };
		int backLayerFillTriangles{ 0 };
	};

	bool isButtonFillTriangle(WorldDrawList::Triangle const& triangle)
	{
		return triangle.texture == WorldDrawList::Texture::None
			&& triangle.colour == kEnabledButtonColour;
	}

	VisibleGeometry visibleGeometry(WorldDrawList const& drawList)
	{
		VisibleGeometry geometry;
		for (auto const& command : drawList.commands())
		{
			if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command))
			{
				if (triangle->texture != WorldDrawList::Texture::None
					|| !survivesClip(boundsOf(triangle->positions, 3, 0.0f), triangle->clip))
					continue;
				if (isButtonFillTriangle(*triangle)) ++geometry.buttonFillTriangles;
				if (triangle->colour == kBackLayerFillColour)
					++geometry.backLayerFillTriangles;
			}
			else if (auto const* line = std::get_if<WorldDrawList::Line>(&command))
			{
				if (line->colour != kEnabledButtonColour) continue;
				ImVec2 const points[2]{ line->from, line->to };
				if (survivesClip(boundsOf(points, 2, line->thickness * 0.5f), line->clip))
					++geometry.buttonOutlineLines;
			}
		}
		return geometry;
	}

	// Button outline commands the renderer recorded, whatever clip they carry.
	// Used to prove the nested threshold pass still runs as the Door opens.
	int recordedButtonOutlineLines(WorldDrawList const& drawList)
	{
		int lines = 0;
		for (auto const& command : drawList.commands())
			if (auto const* line = std::get_if<WorldDrawList::Line>(&command))
				lines += line->colour == kEnabledButtonColour;
		return lines;
	}

	void backButtonRendersAsOutlineOnly()
	{
		// Ticket #195: the check counts only what each command's clip lets
		// through. Door rendering recursively records the far-side Sector inside
		// the Door's aperture, so a closed Door's zero-area clip must not count
		// toward the front Button the way a raw vertex tally did.
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 1280.0f;
		gUISettings.worldViewportHeight = 720.0f;
		gUISettings.worldZoom = 1.0f;
		gUISettings.xOffset = 0.0f;
		gUISettings.yOffset = 0.0f;
		clearObjectTileset();
		Scene scene = buildTwoRoomScene("Back Button rendering");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();

		auto const frontSector = scene.world->getSector(scene.frontSector);
		auto const backSector = scene.world->getSector(scene.backSector);
		ImColor const roomColour(192, 192, 255);

		// The closed Door's aperture is a zero-area clip: the front Button is one
		// visible filled quad (two triangles) and nothing nested reaches the
		// viewport, neither the far-side Button outline nor the Layer behind.
		{
			WorldDrawList drawList(kViewportClip);
			renderSector(frontSector, 0, LayerRenderStyle::Solid, false, roomColour, &drawList);
			auto const geometry = visibleGeometry(drawList);
			require(geometry.buttonFillTriangles == 2,
				"The front Button should render as one filled quad on the Door's authored Layer");
			require(geometry.buttonOutlineLines == 0,
				"A closed Door's zero-area aperture must contribute no visible far-side Button");
			require(geometry.backLayerFillTriangles == 0,
				"A closed Door's zero-area aperture must contribute no visible back-Layer fill");
		}

		// With the Door open the nested Aperture pass must still run: the Layer
		// behind fills the doorway and the far-side Button outline is recorded
		// under a real clip, never dropped. This keeps the check from passing by
		// suppressing valid nested thresholds.
		{
			auto const door = scene.door.object->getDoor();
			door->open();
			while (!door->isOpen()) door->update(door->getOpenCloseTime() * 0.25f);

			WorldDrawList drawList(kViewportClip);
			renderSector(frontSector, 0, LayerRenderStyle::Solid, false, roomColour, &drawList);
			auto const geometry = visibleGeometry(drawList);
			require(geometry.buttonFillTriangles == 2,
				"The open Door must still show one filled front-side Button quad");
			require(geometry.backLayerFillTriangles > 0,
				"An open Door aperture must reveal the Layer behind it");
			require(recordedButtonOutlineLines(drawList) == 4,
				"An open Door aperture must still record the far-side Button outline");
		}

		// The back Layer drawn solid (the back Button's own Layer): per the
		// Door rule, the back Button is still only an outline.
		{
			WorldDrawList drawList(kViewportClip);
			renderSector(backSector, 1, LayerRenderStyle::Solid, false, roomColour, &drawList);
			auto const geometry = visibleGeometry(drawList);
			require(geometry.buttonFillTriangles == 0,
				"The back Button must never render solid, even on its own Layer");
			require(geometry.buttonOutlineLines > 0,
				"The back Button should render as a visible outline on its own Layer");
		}

		// A wide ordinary Door with no right host uses the authored left cell at
		// offset zero. Inspect production draw commands, not mirrored geometry.
		{
			auto options = core::World::RemoteControlledDoor1Options;
			options.width = 2;
			auto fallback = buildTwoRoomScene("Boundary-owned rendering", 2, 4, &options, 4);
			WorldDrawList drawList(kViewportClip);
			renderSector(fallback.world->getSector(fallback.frontSector), 0,
				LayerRenderStyle::Solid, false, roomColour, &drawList);
			float minX = 1e10f, maxX = -1e10f;
			int triangles = 0;
			for (auto const& command : drawList.commands())
				if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
					triangle && isButtonFillTriangle(*triangle))
				{
					++triangles;
					for (auto const& p : triangle->positions) { minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); }
				}
			require(triangles == 2 && std::abs((minX + maxX) * 0.5f - (4.0f * CORE_CELL_WIDTH_PIXELS + gUISettings.worldViewportX + gUISettings.xOffset)) < 0.001f,
				"Production rendering did not centre wide Door fallback on its host boundary");
		}

		// Cross-owner reassignment must move production artwork as well as the
		// interaction approach. Only the Layer-1 authored Button is filled.
		{
			core::World world("Reassigned artwork", 12, 2);
			world.addLayer();
			world.addRoom("Front", 0, 0, 0, 12, 1);
			auto middle = world.addRoom("Middle", 1, 0, 0, 12, 1);
			world.addRoom("Back", 2, 0, 0, 12, 1);
			world.addSectorDoor(1, 0, 3, core::World::RemoteControlledDoor1Options);
			auto check = [&](float expected)
			{
				WorldDrawList drawList(kViewportClip);
				renderSector(world.getSector(middle), 1, LayerRenderStyle::Solid, false, roomColour, &drawList);
				float minX = 1e10f, maxX = -1e10f; int triangles = 0;
				for (auto const& command : drawList.commands())
					if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
						triangle && isButtonFillTriangle(*triangle) && survivesClip(boundsOf(triangle->positions, 3, 0), triangle->clip))
					{
						++triangles;
						for (auto p : triangle->positions) { minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); }
					}
				require(triangles == 2 && std::abs((minX + maxX) * 0.5f - expected * CORE_CELL_WIDTH_PIXELS) < 0.001f,
					"Renderer retained a previous control assignment");
			};
			world.finishBuild(); world.pauseSimulation(); check(4);
			auto options = core::World::RemoteControlledDoor1Options; options.width = 2;
			auto earlier = world.addSectorDoor(0, 0, 2, options);
			world.finishBuild(); check(3);
			world.removeSectorDoor(earlier.door.sector->getIndex(), earlier.door.index);
			world.finishBuild(); check(4);
		}

		// The wireframe overlay of the back Layer, as seen when the front Layer
		// is selected: the back Button shows through as the same outline.
		{
			WorldDrawList drawList(kViewportClip);
			renderSector(backSector, 1, LayerRenderStyle::Wireframe, false, roomColour, &drawList);
			auto const geometry = visibleGeometry(drawList);
			require(geometry.buttonOutlineLines > 0,
				"The back Button should render as an outline in the wireframe overlay");
		}
	}
}

void render_smoke::registerDoorButtons(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "backButtonRendersAsOutlineOnly", isolated<[](smoke::Context const&) { backButtonRendersAsOutlineOnly(); }> });
}
