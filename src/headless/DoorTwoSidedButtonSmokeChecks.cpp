// Two-sided Door Buttons, for ticket #125.
//
// Giving a Door a Button must give it Buttons on both sides of the threshold:
// agents approaching from the Layer behind were stuck, because the single
// Button sat on the front side only. These checks drive the real World
// edit actions and renderer:
//
//   * Add Door Button creates the missing Button on each side, binds both to
//     the Door's single traversal resource with idempotent open-only commands,
//     and records the pre-Button activation mode for removal
//   * the add is all-or-nothing: a side with no space for a Button refuses
//     the whole edit before anything is created
//   * a second add is refused once both sides carry a Button
//   * existing authored/YAML Buttons can be removed, falling back to manual
//     activation when no pre-Button mode was recorded; editor-added Buttons
//     restore the Door's recorded pre-Button activation mode
//   * serialization round-trips both Buttons and the recorded mode
//   * agents on both sides press their side's Button and cross, and every
//     device command either Button issues is an open
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
#include "core/Coordination.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Exceptions.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

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

	std::vector<std::shared_ptr<const core::Button>> buttonsIn(core::World const& world,
		uint32_t sectorIndex)
	{
		std::vector<std::shared_ptr<const core::Button>> buttons;
		auto sector = world.getSector(sectorIndex);
		require(sector != nullptr, "The Sector vanished");
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto const object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::InteractionPoint)
				continue;
			buttons.push_back(std::static_pointer_cast<const core::Button>(object->_getObject()));
		}
		return buttons;
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

	void requireBothSidesHaveOneButton(Scene const& scene, char const* what)
	{
		auto const front = buttonsIn(*scene.world, scene.frontSector);
		auto const back = buttonsIn(*scene.world, scene.backSector);
		require(front.size() == 1 && back.size() == 1,
			std::string(what) + ": the Door should carry exactly one Button per side");
		require(front[0]->getThresholdLayer() == 0 && back[0]->getThresholdLayer() == 0,
			std::string(what) + ": a Door Button did not inherit the Door's authored Layer");
	}

	void addingAButtonGivesBothSidesOne()
	{
		Scene scene = buildTwoRoomScene("Add door button");
		// Add from the back side's registration: the action must not care which
		// side of the pair the selected Sector sits on.
		uint32_t backObjectIndex{ ~0u };
		auto backSector = scene.world->getSector(scene.backSector);
		for (uint32_t i = 0; i < backSector->getNumObjects(); ++i)
			if (backSector->getObject(i) == scene.door.object) backObjectIndex = i;
		require(backObjectIndex != ~0u, "The Door is not registered on its back Sector");

		scene->pauseSimulation();
		require(scene->canAddSectorDoorButton(scene.backSector, backObjectIndex),
			"A Button-less Door should accept an added Door Button");
		scene->addSectorDoorButton(scene.backSector, backObjectIndex);
		scene->finishBuild();

		requireBothSidesHaveOneButton(scene, "Adding a Door Button");
		require(!scene->canAddSectorDoorButton(scene.backSector, backObjectIndex),
			"A both-sided Door should not accept another Door Button");
		require(scene->canRemoveSectorDoorButton(scene.backSector, backObjectIndex),
			"Editor-added Door Buttons should be removable");

		auto const resource = scene->lookupTraversalResource(
			scene.door.object->getDoor()->getTraversalResourceId());
		require(resource && resource.entity->getControls().size() == 2,
			"Both Buttons should bind to the Door's traversal resource");

		core::World::CreateDoorOptions options;
		require(scene->getSectorDoorOptions(0, 0, scene.door.object->getCellX(),
				scene.door.object->getDoor()->getCellsWide(), options),
			"The Door's record could not be read back");
		require(options.controls[0] && options.controls[1]
			&& options.activationMode == core::DoorActivationMode::RemoteControlled,
			"The Door record did not move to remote-controlled with both controls");
	}

	void addingCompletesALegacyOneSidedDoor()
	{
		// A Door authored with only its fore Button, as older builds produced.
		core::World::CreateDoorOptions authored;
		authored.controls[0] = true;
		authored.activationMode = core::DoorActivationMode::RemoteControlled;
		Scene scene = buildTwoRoomScene("Complete a legacy one-sided Door", 8, 0, &authored);
		require(buttonsIn(*scene.world, scene.frontSector).size() == 1
			&& buttonsIn(*scene.world, scene.backSector).empty(),
			"The authored Door should start with exactly its fore Button");

		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();

		requireBothSidesHaveOneButton(scene, "Completing a one-sided Door");
		require(scene->canRemoveSectorDoorButton(scene.frontSector, scene.door.objectIndex),
			"A completed legacy Door should allow its Buttons to be removed");
	}

	void addRefusesWhenASideHasNoSpace()
	{
		// The fore Room is exactly the Door's width, so no cell beside the Door
		// can take a Button on that side.
		Scene scene = buildTwoRoomScene("No space for a Button", 1, 5, nullptr, 5);
		require(scene.door.object->getCellX() == 5
			&& scene.door.object->getDoor()->getCellsWide() == 1,
			"The scene's Door should span its fore Room exactly");

		scene->pauseSimulation();
		auto const buttonsBefore = buttonsIn(*scene.world, scene.frontSector).size()
			+ buttonsIn(*scene.world, scene.backSector).size();
		bool refused = false;
		try
		{
			scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		}
		catch (core::Exception const&)
		{
			refused = true;
		}
		require(refused, "An add with no space on one side should be refused");
		auto const buttonsAfter = buttonsIn(*scene.world, scene.frontSector).size()
			+ buttonsIn(*scene.world, scene.backSector).size();
		require(buttonsBefore == 0 && buttonsAfter == 0,
			"A refused add must not leave a Button behind on either side");
		require(scene->canAddSectorDoorButton(scene.frontSector, scene.door.objectIndex),
			"A refused add should leave the Door addable");
	}

	void secondAddIsRefused()
	{
		Scene scene = buildTwoRoomScene("Second add refused");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();

		bool refused = false;
		try
		{
			scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		}
		catch (core::Exception const&)
		{
			refused = true;
		}
		require(refused, "A both-sided Door should refuse a second add");
		requireBothSidesHaveOneButton(scene, "A refused second add");
	}

	void authoredButtonsCanBeRemoved()
	{
		core::World::CreateDoorOptions authored;
		authored.controls[0] = true;
		authored.controls[1] = true;
		authored.activationMode = core::DoorActivationMode::RemoteControlled;
		Scene scene = buildTwoRoomScene("Authored Buttons", 8, 0, &authored);
		requireBothSidesHaveOneButton(scene, "The authored Door");

		scene->pauseSimulation();
		require(scene->canRemoveSectorDoorButton(scene.frontSector, scene.door.objectIndex),
			"Authored Buttons should read as removable");
		auto const rebuilt = scene->removeSectorDoorButton(scene.frontSector,
			scene.door.objectIndex);
		require(rebuilt != nullptr, "Removing authored Buttons should rebuild the Door");
		require(buttonsIn(*scene.world, scene.frontSector).empty()
			&& buttonsIn(*scene.world, scene.backSector).empty(),
			"Removing authored Buttons should take both away");

		core::World::CreateDoorOptions options;
		require(scene->getSectorDoorOptions(0, 0, rebuilt->getCellX(),
				rebuilt->getDoor()->getCellsWide(), options),
			"The Door record could not be read after removing authored Buttons");
		require(options.activationMode == core::DoorActivationMode::Manual,
			"A Door with removed authored Buttons should fall back to manual activation");
	}

	void removalRemovesBothButtonsAndRestoresTheMode()
	{
		Scene scene = buildTwoRoomScene("Remove door button");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();
		require(scene->canRemoveSectorDoorButton(scene.frontSector, scene.door.objectIndex),
			"Editor-added Buttons should read as removable");

		auto const doorX = scene.door.object->getCellX();
		auto const doorWidth = scene.door.object->getDoor()->getCellsWide();
		auto const rebuilt = scene->removeSectorDoorButton(scene.frontSector,
			scene.door.objectIndex);
		require(rebuilt != nullptr, "Removal should return the rebuilt Door");
		require(rebuilt->getCellX() == doorX && rebuilt->getDoor()->getCellsWide() == doorWidth,
			"Removal returned a Door at the wrong position");

		require(buttonsIn(*scene.world, scene.frontSector).empty()
			&& buttonsIn(*scene.world, scene.backSector).empty(),
			"Removal should take both Buttons away");
		auto const resource = scene->lookupTraversalResource(
			rebuilt->getDoor()->getTraversalResourceId());
		require(resource && resource.entity->getControls().empty(),
			"Removal should unbind every Button from the traversal resource");

		core::World::CreateDoorOptions options;
		require(scene->getSectorDoorOptions(0, 0, doorX, doorWidth, options),
			"The rebuilt Door's record could not be read back");
		require(!options.controls[0] && !options.controls[1]
			&& options.activationMode == core::DoorActivationMode::Manual,
			"Removal should restore the Door's pre-Button activation mode");

		uint32_t rebuiltIndex{ ~0u };
		auto sector = scene.world->getSector(scene.frontSector);
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			if (sector->getObject(i) == rebuilt) rebuiltIndex = i;
		require(rebuiltIndex != ~0u, "The rebuilt Door is not on its front Sector");
		require(scene->canAddSectorDoorButton(scene.frontSector, rebuiltIndex),
			"The rebuilt Button-less Door should accept Buttons again");
		require(!scene->canRemoveSectorDoorButton(scene.frontSector, rebuiltIndex),
			"The rebuilt Door should not read as removable");
	}

	void serializationPreservesButtonsAndProvenance()
	{
		Scene scene = buildTwoRoomScene("Serialize two-sided Buttons");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		scene->serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("preButtonActivation") != std::string::npos,
			"The pre-Button activation mode did not persist");

		auto loaded = std::make_unique<core::World>("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		loaded->deserialize(*reader, workData);

		auto const loadedDoor = findDoor(*loaded);
		require(loadedDoor.object != nullptr, "The loaded World has no Door");
		require(buttonsIn(*loaded, loadedDoor.object->getDoor()->getFrontSector()->getIndex()).size() == 1
			&& buttonsIn(*loaded, loadedDoor.object->getDoor()->getBackSector()->getIndex()).size() == 1,
			"The loaded Door did not keep one Button per side");
		require(loaded->canRemoveSectorDoorButton(loadedDoor.sectorIndex, loadedDoor.objectIndex),
			"The loaded Door lost its Buttons' removal provenance");
	}

	void agentsOnBothSidesPressAndCross()
	{
		Scene scene = buildTwoRoomScene("Cross from both sides");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();
		require(scene->resumeSimulation(), "The scene should resume after its edit");
		auto const door = scene.door.object->getDoor();
		auto const doorResource = door->getTraversalResourceId();

		auto graph = scene->getGraph();
		auto foreTarget = graph->getClosestVertexInSector(
			scene.world->getSector(scene.frontSector).get(), { 2.0f, 0.0f });
		auto backTarget = graph->getClosestVertexInSector(
			scene.world->getSector(scene.backSector).get(), { 13.0f, 0.0f });
		require(foreTarget && backTarget, "The scene needs a target vertex on each side");

		auto const foreWalker = scene->createAgent("Fore walker", scene.frontSector, 0, 2.0f);
		auto const backWalker = scene->createAgent("Back walker", scene.backSector, 0, 13.0f);
		for (auto const& [id, target] : { std::pair{ foreWalker, backTarget },
			std::pair{ backWalker, foreTarget } })
		{
			auto agent = scene->lookupAgent(id).entity;
			require(agent != nullptr, "An Agent could not be created");
			auto path = graph->calculatePath(agent, target);
			require(path != nullptr, "An Agent's path could not be calculated");
			agent->setPath(path, true);
		}

		bool sawDoorOpen = false;
		bool sawOpenCommand = false;
		bool sawCloseCommand = false;
		for (uint32_t tick = 0; tick < 6000; ++tick)
		{
			scene->advanceTick();
			sawDoorOpen = sawDoorOpen || door->isOpen();
			auto const snapshot = scene->getSimulationSnapshot();
			for (auto const& operation : snapshot.deviceOperations)
			{
				if (operation.command.type != core::DeviceCommandType::OpenDoor
					|| operation.command.traversalResource != doorResource) continue;
				sawOpenCommand = sawOpenCommand || operation.command.desiredState;
				sawCloseCommand = sawCloseCommand || !operation.command.desiredState;
			}
			auto const foreIdle = scene->lookupAgent(foreWalker).entity->getState()
				== core::Agent::State::Idle;
			auto const backIdle = scene->lookupAgent(backWalker).entity->getState()
				== core::Agent::State::Idle;
			if (foreIdle && backIdle) break;
		}

		auto const foreAgent = scene->lookupAgent(foreWalker).entity;
		auto const backAgent = scene->lookupAgent(backWalker).entity;
		require(foreAgent->getState() == core::Agent::State::Idle
			&& backAgent->getState() == core::Agent::State::Idle,
			"Agents on both sides should finish their crossing");
		require(foreAgent->getGlobalPosition().distanceTo(backTarget->getPosition()) < 0.01f
			&& backAgent->getGlobalPosition().distanceTo(foreTarget->getPosition()) < 0.01f,
			"Agents on both sides should arrive at their destinations");
		require(sawDoorOpen, "The Door should have opened for the crossings");
		require(sawOpenCommand, "The Buttons should have issued open commands");
		require(!sawCloseCommand,
			"Neither Button may ever issue a close command: pressing the other one again must not close the Door");
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

void runDoorTwoSidedButtonSmokeChecks()
{
	addingAButtonGivesBothSidesOne();
	addingCompletesALegacyOneSidedDoor();
	addRefusesWhenASideHasNoSpace();
	secondAddIsRefused();
	authoredButtonsCanBeRemoved();
	removalRemovesBothButtonsAndRestoresTheMode();
	serializationPreservesButtonsAndProvenance();
	agentsOnBothSidesPressAndCross();
	backButtonRendersAsOutlineOnly();
}
