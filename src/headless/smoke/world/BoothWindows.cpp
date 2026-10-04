#include "Checks.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/Pathing.h"
#include "core/WindowVertex.h"
#include "core/Button.h"
#include "core/YamlSerializer.h"
#include <cmath>
#include <limits>
#include "../support/DumbwaiterFixture.h"

namespace
{
	using smoke::require;
	std::string saved(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
		work.markSerializedUnmodified = false; world.serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}
	void dumbwaiters(smoke::Context const&)
	{
		using namespace dumbwaiter_fixture;
		// Both standard Buttons use one shared side, and placement is atomic.
		for (unsigned layout = 0; layout < 7; ++layout)
		{
			core::World world("Landing borders", 6, 3);
			world.addRoom("Lower", 0, 0, layout == 1 || layout == 3 || layout == 4 ? 2 : 0,
				layout == 3 ? 1 : 4, 1);
			world.addCorridor(0, 1, layout == 2 || layout == 6 ? 2 : layout == 4 ? 1 : 0,
				layout == 4 ? 2 : layout == 6 ? 1 : 4, 1);
			if (layout == 5) world.addDumbwaiter(1, 0, 2);
			world.finishBuild(); world.pauseSimulation();
			auto before = yaml(world); std::string diagnostic;
			bool legal = layout < 3;
			require(world.canAddDumbwaiter(1, 0, 2, {}, &diagnostic) == legal,
				"Incorrect shared Button border or one-cell landing preflight");
			if (!legal)
			{
				bool refused = false;
				try { world.addDumbwaiter(1, 0, 2); } catch (std::exception const&) { refused = true; }
				require(refused && yaml(world) == before, "Illegal Button layout mutated the World");
				continue;
			}
			auto id = world.addDumbwaiter(1, 0, 2); world.finishBuild();
			auto unit = world.lookupDumbwaiter(id);
			for (uint32_t stop = 0; stop < 2; ++stop)
			{
				auto point = world.lookupInteractionPoint(unit->getLandingButton(stop)).entity;
				require(point->getPosition() == core::Vector2{layout == 0 ? 2.0f : 3.0f, float(stop)},
					"Landing Buttons did not choose left by default or shared right fallback");
				auto landing = unit->getStop(stop).sector;
				bool found = false;
				for (uint32_t i = 0; i < landing->getNumObjects(); ++i)
					if (auto object = landing->getObject(i); world.isDumbwaiterOwnedControl(object))
					{
						auto button = std::dynamic_pointer_cast<const core::Button>(object->_getObject());
						require(button && button->getSize().x == CORE_BUTTON_SIZE
							&& button->getInteractionPointId() == unit->getLandingButton(stop),
							"Landing control is not a standard physical Button");
						require(!world.planMoveSectorObject(landing->getIndex(), i, 1, stop).valid,
							"Owned Button moved independently");
						found = true;
					}
				require(found, "Landing has no standard owned Button object");
			}
		}
		for (unsigned kind = 0; kind < 3; ++kind) for (bool shared : {false, true})
			for (uint32_t layer : {1u, 2u, 3u}) for (uint32_t initial : {0u, 1u})
			{
				auto world = make(kind, shared, layer);
				auto id = world->addDumbwaiter(layer, 0, 2, {initial, 2}); world->finishBuild();
				auto unit = world->lookupDumbwaiter(id);
				require(unit && unit->getCellsWide() == 1 && unit->getLevelsHigh() == 2
					&& unit->getNumStops() == 2 && unit->getCapacity() == 0
					&& unit->getInitialStop() == initial && unit->getTravelSeconds() == 2
					&& unit->getCarPosition().y == float(initial), "Wrong authored unit/defaults");
				for (uint32_t stop = 0; stop < 2; ++stop)
				{
					auto aperture = unit->getAperture(stop);
					require(aperture && aperture->getDumbwaiterOwner() == id && !aperture->getPanel()
						&& aperture->getFrontLayer() == layer - 1 && aperture->getBackLayer() == layer
						&& aperture->getProgress() == (stop == initial ? 1.0f : 0.0f)
						&& !aperture->isTraversalConfigured(), "Wrong owned aperture/initial shutter");
					core::DeviceCommand command; command.type = core::DeviceCommandType::ToggleBoothWindow;
					command.boothWindow = aperture->getDeviceId();
					require(!world->submitDeviceCommand(command), "Owned shutter accepted independent toggle");
				}
				for (auto const& vertex : world->getGraph()->getVertices())
					require(vertex->getSector() != unit, "Shaft acquired a walkable approach");
				std::string diagnostic;
				require(!world->canPlaceAgentInLocation(unit->getIndex(), {}, {}, &diagnostic), "Agent can enter shaft");
				auto snapshot = world->getSimulationSnapshot();
				require(snapshot.traversalResources.empty() && snapshot.interactionPoints.size() == 2
					&& snapshot.deviceOperations.empty(), "Unit introduced passenger/control work");
				world->pauseSimulation(); auto before = yaml(*world); world->markSaved();
				for (float seconds : {0.0f, 0.09f, 60.1f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
				{
					bool refused = false; try { world->configureDumbwaiter(id, {0, seconds}); }
					catch (std::exception const&) { refused = true; }
					require(refused && yaml(*world) == before && !world->isModified(), "Invalid timing mutated World");
				}
				for (float seconds : {0.1f, 60.0f})
				{
					require(world->configureDumbwaiter(id, {1, seconds}), "Timing endpoint refused");
					require(world->lookupDumbwaiter(id)->getTravelSeconds() == seconds, "Timing endpoint lost");
				}
				world->resetSimulation(); world->pauseSimulation();
				require(world->lookupDumbwaiter(id)->getAperture(1)->getProgress() == 1, "Reset lost authored initial Stop");
				auto removal = kind == 2 ? world->planRemoveFacade(0) : world->planRemoveLocation(0);
				require(removal.valid, "Dependent landing removal preflight refused: " + removal.diagnostic);
				require(world->removeDumbwaiter(id) && !world->lookupDumbwaiter(id), "Whole unit deletion failed");
				for (uint32_t sector = 0; sector < world->getNumSectors(); ++sector)
					for (uint32_t object = 0; object < world->getSector(sector)->getNumObjects(); ++object)
						require(!std::dynamic_pointer_cast<const core::WindowSectorObject>(world->getSector(sector)->getObject(object)), "Deletion left orphan aperture");
				auto replacement = world->addDumbwaiter(layer, 0, 2); world->finishBuild();
				require(replacement != id && world->lookupDumbwaiter(replacement)->getInitialStop() == 0, "Identity reused/default not lower");
			}
		{
			core::World world("Force Bridge refusal", 6, 3);
			auto room = world.addRoom("Landing", 0, 0, 0, 5, 2);
			world.addSectorWalkway(room, 1, 1); world.addSectorWalkway(room, 1, 3);
			world.addSectorForceBridge(room, 1, 2, {1, CORE_SIDE_LEFT, true, true, 1});
			world.finishBuild(); world.pauseSimulation(); auto before = yaml(world);
			std::string diagnostic; require(!world.canAddDumbwaiter(1, 0, 2, {}, &diagnostic), "Force Bridge support admitted");
			bool refused = false; try { world.addDumbwaiter(1, 0, 2); } catch (std::exception const&) { refused = true; }
			require(refused && yaml(world) == before, "Force Bridge refusal mutated World");
		}
		{
			core::World world("Upper bound", 6, 4);
			world.addRoom("Lower", 0, 2, 2, 2, 1); world.addFacade(0, 3, 2, 2, 1);
			auto id = world.addDumbwaiter(1, 2, 2); world.finishBuild(); world.pauseSimulation();
			require(world.lookupDumbwaiter(id)->getCarPosition().y == 2, "Valid upper World bound refused");
			auto before = yaml(world);
			bool refused = false; try { world.configureDumbwaiter(id, {2, 2}); } catch (std::exception const&) { refused = true; }
			require(refused && yaml(world) == before, "Invalid initial Stop mutated World");
			require(!world.planDeleteLayer(0).valid && world.planDeleteLevel(2).valid, "Dimension edit preflight incorrect");
		}
		{
			auto world = make(); auto id = world->addDumbwaiter(1, 0, 2); world->finishBuild(); world->pauseSimulation();
			auto stale = world->lookupDumbwaiter(id)->getAperture(0)->getDeviceId();
			auto plan = world->planRemoveSectorWalkway(0, 0);
			require(plan.valid && !plan.consequences.empty() && world->applyWalkwayEdit(plan)
				&& !world->lookupDumbwaiter(id) && !world->lookupBoothWindow(stale), "Support removal left a corrupt unit");
		}
		{
			auto world = make(); auto first = world->addDumbwaiter(1, 0, 2);
			auto landing = world->addRoom("Second landing", 0, 0, 4, 2, 2);
			world->addSectorWalkway(landing, 1, 0); auto second = world->addDumbwaiter(1, 0, 4);
			world->addRoom("Standalone front", 0, 0, 0, 2, 1);
			world->addRoom("Standalone back", 1, 0, 0, 2, 1);
			auto standalone = world->addBoothWindow(0, 0, 0); world->finishBuild(); world->pauseSimulation();
			auto plan = world->planMoveSectorObject(standalone.window.sector->getIndex(), standalone.window.index, 1, 0);
			require(plan.valid, "Dumbwaiter changed unrelated standalone movement: " + plan.diagnostic);
			world->applyObjectMove(plan);
			require(world->lookupDumbwaiter(first) && world->lookupDumbwaiter(second), "Surrounding edit lost unit identities");
			require(world->removeDumbwaiter(first) && world->lookupDumbwaiter(second)
				&& world->lookupDumbwaiter(second)->getAperture(0)->getDumbwaiterOwner() == second,
				"Deleting one unit corrupted later producers or sibling ownership");
			require(world->removeDumbwaiter(second), "Sibling deletion failed");
		}
		for (unsigned kind = 0; kind < 3; ++kind) for (uint32_t landing : {0u, 1u})
		{
			auto world = make(kind, false, 2); world->addLayer();
			auto id = world->addDumbwaiter(2, 0, 2);
			auto other = world->addRoom("Other", 3, 0, 4, 1, 1);
			world->addSectorMarker(other, 0, 0.5f); world->finishBuild(); world->pauseSimulation();
			auto plan = kind == 2 ? world->planRemoveFacade(landing) : world->planRemoveLocation(landing);
			require(plan.valid && !plan.consequences.empty(), "Each landing deletion preflight failed: " + plan.diagnostic);
			world->applyLocationEdit(plan);
			require(!world->lookupDumbwaiter(id) && world->getSimulationSnapshot().interactionPoints.empty(), "Each landing deletion left dependent children");
			world->resetSimulation(); world->pauseSimulation();
			auto otherSector = world->getSectorAtPosition(3, 4, 0);
			require(!world->lookupDumbwaiter(id) && otherSector && otherSector->getNumObjects() == 1
				&& otherSector->getObject(0)->getObjectType() == core::SectorObjectType::Marker, "Deletion remapping lost unrelated authored object");
		}
		{
			core::World world("Moved support", 6, 3);
			auto room = world.addRoom("Landing", 0, 0, 2, 2, 2);
			world.addSectorWalkway(room, 1, 0);
			auto id = world.addDumbwaiter(1, 0, 2);
			world.addSectorMarker(room, 0, 1.5f); world.finishBuild(); world.pauseSimulation();
			auto plan = world.planMoveSectorObject(room, 0, 3, 1);
			require(plan.valid, "Supported surrounding Walkway move refused: " + plan.diagnostic);
			world.applyObjectMove(plan);
			require(!world.lookupDumbwaiter(id) && world.getSimulationSnapshot().interactionPoints.empty(), "Support relocation left unsupported unit");
			world.resetSimulation(); world.pauseSimulation();
			auto landing = world.getSectorAtPosition(0, 2, 0);
			require(landing && landing->getObject(5) && landing->getObject(5)->getObjectType() == core::SectorObjectType::Marker,
				"Dependent deletion shifted later object slots");
		}
		for (uint32_t level : {0u, 1u})
		{
			core::World world("Level compaction", 6, 5); world.addLayer();
			world.addRoom("Lower", 1, 2, 2, 2, 1); world.addCorridor(1, 3, 2, 2, 1);
			auto id = world.addDumbwaiter(2, 2, 2, {1, 0.5f}); world.finishBuild(); world.pauseSimulation();
			world.pressDumbwaiterLanding(id, 0);
			world.applyDeleteLevel(world.planDeleteLevel(level));
			auto unit = world.lookupDumbwaiter(id);
			require(unit && unit->getCellY() == 1 && unit->getCarPosition().y == 2 && unit->getStop(0).sector->getCellY() == 1
				&& unit->getStop(1).sector->getCellY() == 2 && unit->getAperture(1)->getProgress() == 1, "Level compaction broke Stop adjacency/initial state");
			world.resetSimulation(); world.pauseSimulation();
			require(world.lookupDumbwaiter(id)->getCellY() == 1, "Level compaction not persisted in replay");
		}
		{
			auto world = make(); auto id = world->addDumbwaiter(1, 0, 2); world->finishBuild();
			auto actor = world->createAgent("Support occupant", 0, 1, 0.5f);
			auto operation = world->pressDumbwaiterLanding(id, 1);
			auto before = yaml(*world); bool refused = false;
			try { world->applyWalkwayEdit(world->planRemoveSectorWalkway(0, 0)); } catch (std::exception const&) { refused = true; }
			require(refused && yaml(*world) == before && world->lookupDumbwaiter(id)->getOperation() == operation
				&& world->lookupAgent(actor), "Rejected support edit deleted/cancelled dependent unit");
		}
		for (unsigned failure = 0; failure < 7; ++failure)
		{
			auto world = make();
			if (failure == 0) world->addBackground(1, 0, 2, 1, 2);
			if (failure == 1) world->removeSectorWalkway(0, 0);
			if (failure == 2)
			{
				world->addRoom("Back", 1, 0, 2, 1, 2);
				world->addSectorWindow(0, 0, 2, 1, 1);
			}
			world->finishBuild(); world->pauseSimulation(); world->markSaved(); auto before = yaml(*world);
			auto layer = failure == 3 ? 0u : failure == 4 ? 99u : 1u;
			auto y = failure == 5 ? 3u : 0u, x = failure == 6 ? 6u : 2u;
			std::string diagnostic; require(!world->canAddDumbwaiter(layer, y, x, {}, &diagnostic) && !diagnostic.empty(), "Invalid placement accepted");
			bool refused = false; try { world->addDumbwaiter(layer, y, x); } catch (std::exception const&) { refused = true; }
			require(refused && yaml(*world) == before && !world->isModified() && world->isTraversalTopologyValid(), "Placement refusal mutated World");
		}
	}
	void canonicalDumbwaiterReplay();
	void dumbwaiterMoves(smoke::Context const&)
	{
		canonicalDumbwaiterReplay();
		using namespace dumbwaiter_fixture;
		for (unsigned ticks : {0u, 12u, 60u, 100u})
		{
			auto world = make(); addLandings(*world,1,4);
			auto id = world->addDumbwaiter(1,0,2,{0,0.5f}); world->finishBuild();
			auto operation = world->pressDumbwaiterLanding(id,0);
			world->resumeSimulation(); require(world->advanceTicks(ticks), "Phase move fixture failed"); world->pauseSimulation();
			require(world->lookupDumbwaiter(id)->isBusy() && world->applyDumbwaiterMove(world->planMoveDumbwaiter(id,1,0,4))
				&& world->lookupDeviceOperation(operation).entity->getState() == core::DeviceOperationState::Cancelled
				&& world->lookupDumbwaiter(id)->getCarPosition() == core::Vector2{4,0}, "Move failed cancellation in an accepted phase");
		}
		{
			auto world = make(); addLandings(*world,1,4);
			auto id = world->addDumbwaiter(1,0,2); world->finishBuild();
			auto stale = world->planMoveDumbwaiter(id,1,0,4); require(stale.valid, stale.diagnostic);
			world->addBackground(1,0,4,1,1);
			world->addSectorWindow(0,0,4,1,1); world->finishBuild();
			auto operation = world->pressDumbwaiterLanding(id,0); auto before = yaml(*world); bool refused = false;
			try { world->applyDumbwaiterMove(stale); } catch (std::exception const&) { refused = true; }
			require(refused && yaml(*world) == before && world->lookupDumbwaiter(id)->getOperation() == operation,
				"Stale move plan ignored new landing conflict/cancelled accepted work");
		}
		{
			auto world = make(); world->addCorridor(0,2,2,2,1);
			auto id = world->addDumbwaiter(1,0,2); world->finishBuild();
			require(world->applyDumbwaiterMove(world->planMoveDumbwaiter(id,1,1,2)), "Overlapping self footprint movement refused");
			world->resetSimulation(); world->pauseSimulation();
			require(world->lookupDumbwaiter(id)->getCarPosition() == core::Vector2{2,1}
				&& !world->getSectorAtPosition(1,2,0), "Overlapping movement/replay retained old shaft row");
		}
		for (unsigned kind = 0; kind < 3; ++kind)
		{
			auto world = make(kind);
			addLandings(*world, 1, 4, 0, kind); addLandings(*world, 1, 0);
			addLandings(*world, 3, 0, 2, kind);
			auto id = world->addDumbwaiter(1, 0, 2, {1, 0.5f});
			auto sibling = world->addDumbwaiter(1, 0, 0); world->finishBuild();
			auto lower = world->addAccessPermission("Lower"), upper = world->addAccessPermission("Upper");
			auto unit = world->lookupDumbwaiter(id);
			world->setInteractionPointPermissionRequirement(unit->getLandingButton(0), {lower});
			world->setInteractionPointPermissionRequirement(unit->getLandingButton(1), {upper});
			auto operation = world->pressDumbwaiterLanding(id, 0);
			auto otherOperation = world->pressDumbwaiterLanding(sibling, 1);
			world->resumeSimulation(); require(world->advanceTicks(60), "Move cycle setup failed"); world->pauseSimulation();
			auto other = world->lookupDumbwaiter(sibling); auto otherPosition = other->getCarPosition();
			auto before = yaml(*world); auto position = unit->getCarPosition(); world->markSaved();
			for (auto destination : {std::array<uint32_t,3>{0,0,4}, {4,0,4}, {1,3,4}, {1,0,6}, {1,0,0}, {1,2,4}})
			{
				auto plan = world->planMoveDumbwaiter(id, destination[0], destination[1], destination[2]);
				require(!plan.valid && !plan.diagnostic.empty(), "Invalid unit move preflight accepted");
				bool refused = false; try { world->applyDumbwaiterMove(plan); } catch (std::exception const&) { refused = true; }
				require(refused && yaml(*world) == before && !world->isModified() && unit->getCarPosition() == position
					&& unit->getOperation() == operation && unit->isBusy(), "Refused move cancelled/mutated accepted cycle");
			}
			require(!world->applyDumbwaiterMove(world->planMoveDumbwaiter(id, 1, 0, 2)) && unit->getOperation() == operation,
				"Same-position move reset a cycle");
			auto staleBooth = unit->getAperture(0)->getDeviceId(); auto staleButton = unit->getLandingButton(0);
			require(world->applyDumbwaiterMove(world->planMoveDumbwaiter(id, 1, 0, 4)), "Valid unit movement refused");
			unit = world->lookupDumbwaiter(id);
			require(!unit->isBusy() && unit->getCarPosition() == core::Vector2{4,1}
				&& unit->getAperture(1)->getProgress() == 1 && unit->getAperture(0)->getProgress() == 0
				&& world->lookupDeviceOperation(operation).entity->getState() == core::DeviceOperationState::Cancelled,
				"Move did not restore authored Stop/cancel its operation");
			require(!world->lookupBoothWindow(staleBooth) && !world->lookupInteractionPoint(staleButton)
				&& !world->getSectorAtPosition(1,2,0) && other->getCarPosition() == otherPosition
				&& other->getOperation() == otherOperation && other->isBusy(), "Move left stale ownership or reset unrelated unit");
			for (uint32_t stop = 0; stop < 2; ++stop)
			{
				auto point = world->lookupInteractionPoint(unit->getLandingButton(stop)).entity;
				require(point->getPosition() == core::Vector2{5.0f,float(stop)}
					&& world->getInteractionPointPermissionRequirement(unit->getLandingButton(stop))
					== std::vector<core::AccessPermissionId>{stop == 0 ? lower : upper}, "Move lost button placement/requirement");
			}
			auto actor = world->createAgent("New landing operator", unit->getStop(1).sector->getIndex(), 0, 1.0f);
			world->grantAgentAccessPermission(actor, upper);
			auto request = world->requestDumbwaiterLanding(id, 1, actor);
			require(bool(request), "Moved unit Agent request refused");
			// A second successful move also cancels a pending press, before activation.
			require(world->applyDumbwaiterMove(world->planMoveDumbwaiter(id, 3, 2, 0)), "Different Layer-pair movement refused");
			require(world->lookupInteractionRequest(request).entity->getResult() == core::InteractionResult::Cancelled,
				"Move retained pending landing request");
			unit = world->lookupDumbwaiter(id);
			require(unit->getCarPosition() == core::Vector2{0,3} && unit->getAperture(0)->getFrontLayer() == 2
				&& !world->getSectorAtPosition(1,4,0), "Different Layer-pair move left old shaft");
			auto press = world->pressDumbwaiterLanding(id, 0); world->resumeSimulation();
			require(world->advanceTicks(126) && world->lookupDeviceOperation(press).entity->getState() == core::DeviceOperationState::Succeeded,
				"Moved unit user control is unsafe"); world->pauseSimulation();
			world->resetSimulation(); world->pauseSimulation(); unit = world->lookupDumbwaiter(id);
			require(unit->getLayerIndex() == 3 && unit->getCellY() == 2 && unit->getAperture(1)->getProgress() == 1,
				"Construction replay lost movement");
			require(world->removeDumbwaiter(id), "Moved unit deletion failed"); world->resetSimulation(); world->pauseSimulation();
			require(!world->lookupDumbwaiter(id) && world->lookupDumbwaiter(sibling), "Moved-unit deletion replay broke sibling");
		}
	}
	void canonicalDumbwaiterReplay()
	{
		using namespace dumbwaiter_fixture;
		auto world = make(); auto first = world->addDumbwaiter(1, 0, 2, {1, 0.5f});
		addLandings(*world, 1, 4);
		world->addCorridor(0, 0, 0, 2, 1); world->addCorridor(0, 1, 0, 2, 1);
		world->finishBuild(); world->pauseSimulation();
		require(world->applyDumbwaiterMove(world->planMoveDumbwaiter(first, 1, 0, 4)), "Canonical replay move fixture failed");
		auto second = world->addDumbwaiter(1, 0, 2);
		auto ladder = world->addLadder(1, 0, 0, {2, false, true}); world->finishBuild(); world->pauseSimulation();
		auto plan = world->planResizeLadder(ladder.ladder.sector->getIndex(), 0, 0, {2, false, true});
		require(plan.valid, "Canonical construction preflight lost Dumbwaiter chronology: " + plan.diagnostic);
		world->applyLadderEdit(plan);
		require(world->lookupDumbwaiter(first)->getCellX() == 4 && world->lookupDumbwaiter(second)->getCellX() == 2,
			"Canonical construction replay lost complete unit placements");
		world->resetSimulation(); world->pauseSimulation();
		require(world->lookupDumbwaiter(first)->getAperture(1)->getProgress() == 1 && world->lookupDumbwaiter(second), "Canonical Reset lost coherent units");
		world->applyWalkwayEdit(world->planRemoveSectorWalkway(0, 0));
		require(world->lookupDumbwaiter(first) && !world->lookupDumbwaiter(second), "Support edit lost surviving moved unit");
		for (unsigned edit = 0; edit < 5; ++edit)
		{
			auto movedWorld = make(0, true, 2);
			auto id = movedWorld->addDumbwaiter(2, 0, 2, {1, 0.37f});
			addLandings(*movedWorld, 4, 4, 2); movedWorld->finishBuild(); movedWorld->pauseSimulation();
			require(movedWorld->applyDumbwaiterMove(movedWorld->planMoveDumbwaiter(id, 4, 2, 4)), "Historical landing fixture move failed");
			if (edit == 0) movedWorld->applyWalkwayEdit(movedWorld->planRemoveSectorWalkway(0, 0));
			if (edit == 1) movedWorld->applyLocationEdit(movedWorld->planRemoveLocation(0));
			if (edit == 2) movedWorld->applyDeleteLevel(movedWorld->planDeleteLevel(0));
			if (edit == 3 || edit == 4) movedWorld->applyDeleteLayer(movedWorld->planDeleteLayer(edit - 2));
			auto expectedLayer = edit >= 3 ? 3u : 4u, expectedY = edit == 2 ? 1u : 2u;
			auto unit = movedWorld->lookupDumbwaiter(id);
			require(unit && unit->getLayerIndex() == expectedLayer && unit->getCellY() == expectedY
				&& unit->getInitialStop() == 1 && unit->getTravelSeconds() == 0.37f, "Obsolete landing removal lost supported final placement");
			movedWorld->resetSimulation(); movedWorld->pauseSimulation();
			core::World loaded("Edited moved unit", 1, 1); auto input = core::YamlSerializer::fromString(yaml(*movedWorld));
			input->deserialize(); core::SerializationWorkData work;
			require(loaded.deserialize(*input, work) && loaded.lookupDumbwaiter(id)->getLayerIndex() == expectedLayer
				&& loaded.lookupDumbwaiter(id)->getCellY() == expectedY, "Edited final placement could not replay/reopen");
		}
	}

	void placement(smoke::Context const&)
	{
		for (uint32_t front : {0u, 1u, 2u}) for (unsigned a = 0; a < 3; ++a) for (unsigned b = 0; b < 3; ++b)
			for (auto state : {core::Window::State::Closed, core::Window::State::Open})
			{
				core::World world("BoothWindow pairs", 8, 3); world.addLayer(); world.addLayer();
				auto location = [&](uint32_t layer, unsigned kind) {
					if (kind == 0) return world.addRoom("Room", layer, 0, 0, 7, 2);
					if (kind == 1) return world.addCorridor(layer, 0, 0, 7, 1);
					return world.addFacade(layer, 0, 0, 7, 2);
				};
				auto left = location(front, a), right = location(front + 1, b);
				auto created = world.addBoothWindow(front, 0, 3, state); world.finishBuild();
				require(created.window.type == core::SectorObjectType::BoothWindow && created.object->isBoothWindow()
					&& created.object->getState() == state && !created.object->isTraversalConfigured()
					&& !created.traversalResource && world.getSimulationSnapshot().traversalResources.empty()
					&& world.getSimulationSnapshot().interactionPoints.size() == 1, "BoothWindow leaked traversal or duplicated panel resources");
				auto booth = std::static_pointer_cast<const core::BoothWindow>(created.object);
				auto panel = world.lookupInteractionPoint(booth->getPanel()).entity;
				require(panel && panel->getSector() == core::SectorId{static_cast<uint64_t>(right) + 1}
					&& panel->getPosition().x == 3.5f && panel->getPosition().y == 0
					&& panel->getDurationTicks() == 1 && panel->getReach() == 0.25f,
					"Panel not at centred back-side walkable approach");
				auto position = created.object->getPosition(); auto size = created.object->getSize();
				require(std::abs(position.x - 3.1f) < 0.001f && std::abs(position.y - 0.2f) < 0.001f
					&& std::abs(size.x - 0.8f) < 0.001f && std::abs(size.y - 0.3f) < 0.001f, "Wrong aperture geometry");
				unsigned count = 0;
				for (auto const& vertex : world.getGraph()->getVertices())
					if (auto point = std::dynamic_pointer_cast<const core::WindowVertex>(vertex); point && point->getWindow() == created.object)
					{
						++count; auto p = point->getPosition();
						require(std::abs(p.x - 3.5f) < 0.001f && std::abs(p.y) < 0.001f, "Approach is not centred on walking Level");
					}
				require(count == 2, "BoothWindow must have exactly two approaches");
				world.pauseSimulation(); uint32_t from, to;
				world.addSectorMarker(left, 0, 0.5f, &from); world.addSectorMarker(right, 0, 0.5f, &to); world.finishBuild();
				auto graph = world.getGraph();
				require(!core::pathing::findPath(nullptr, graph.get(), graph->getVertexByIdentifier(from), graph->getVertexByIdentifier(to)), "BoothWindow allowed crossing");
				for (auto const& vertex : graph->getVertices())
					if (auto point = std::dynamic_pointer_cast<const core::WindowVertex>(vertex); point && point->getWindow() == created.object)
						require(bool(core::pathing::findPath(nullptr, graph.get(), graph->getVertexByIdentifier(point->getSector()->getIndex() == left ? from : to), point)), "Approach disconnected from same-Layer walking topology");
				world.addSectorWindow(front, 0, 5, 1, 1, {true, core::Window::State::Open, core::Window::Style::Clear}); world.finishBuild(); graph = world.getGraph();
				require(bool(core::pathing::findPath(nullptr, graph.get(), graph->getVertexByIdentifier(from), graph->getVertexByIdentifier(to))), "Ordinary Window positive crossing control failed");
			}
	}
	void simultaneousPairs(smoke::Context const&)
	{
		core::World world("Stacked BoothWindows",8,2); world.addLayer(); world.addLayer();
		for (uint32_t layer=0;layer<4;++layer) world.addRoom("Room",layer,0,0,7,1);
		for (uint32_t layer=0;layer<3;++layer) world.addBoothWindow(layer,0,3,
			layer==1 ? core::Window::State::Open : core::Window::State::Closed);
		world.finishBuild();
		std::array<unsigned,4> counts{};
		for (auto const& vertex : world.getGraph()->getVertices())
			if (auto point=std::dynamic_pointer_cast<const core::WindowVertex>(vertex); point && point->getWindow()->isBoothWindow())
				++counts[point->getSector()->getLayerIndex()];
		require(counts==std::array<unsigned,4>{1,2,2,1},"Overlapping adjacent pairs lost an approach or duplicated one");
		for (uint32_t from=0;from<4;++from) for (uint32_t to=from+1;to<4;++to)
		{
			auto a=world.getGraph()->getVertexAtPosition(from,3.5f,0,0.01f);
			auto b=world.getGraph()->getVertexAtPosition(to,3.5f,0,0.01f);
			require(a && b && !core::pathing::findPath(nullptr,world.getGraph().get(),a,b),"Stacked BoothWindows paired across Layers");
		}
	}

	void lifecycle(smoke::Context const&)
	{
		for (unsigned operation=0;operation<6;++operation)
		{
			core::World world("BoothWindow lifecycle",10,4); world.addLayer(); world.addLayer();
			world.addRoom("Front",1,0,0,8,3); world.addRoom("Back",2,0,0,8,3);
			world.addSectorWalkway(0,1,3); world.addSectorWalkway(1,1,3);
			world.addBoothWindow(1,1,3,core::Window::State::Open); world.finishBuild(); world.pauseSimulation();
			if (operation==0) world.applyLocationEdit(world.planResizeLocation(1,0,0,9,3));
			if (operation==1) world.applyLocationEdit(world.planResizeLocation(1,0,0,3,3));
			if (operation==2) world.applyLocationEdit(world.planRemoveLocation(1));
			if (operation==3) world.applyDeleteLayer(world.planDeleteLayer(0));
			if (operation==4) world.applyDeleteLayer(world.planDeleteLayer(2));
			if (operation==5)
			{
				try { world.removeSectorWalkway(1,0); }
				catch (std::exception const& error) { throw smoke::Failure(std::string("Walkway support removal: ")+error.what()); }
			}
			unsigned approaches=0;
			for (auto const& vertex : world.getGraph()->getVertices())
				if (auto point=std::dynamic_pointer_cast<const core::WindowVertex>(vertex); point && point->getWindow()->isBoothWindow())
				{
					++approaches;
					require(point->getWindow()->getBackLayer()==point->getWindow()->getFrontLayer()+1,"Lifecycle broke adjacent pair");
				}
			require(world.getSimulationSnapshot().interactionPoints.size() == ((operation==0 || operation==3) ? 1u : 0u),
				"Lifecycle retained invalid panel or lost reconstructed panel");
			for (auto const& panel : world.getSimulationSnapshot().interactionPoints)
				require(panel.position.x == 3.5f && panel.position.y == 1,
					"Reconstructed panel lost walkable approach position");
			require(approaches==((operation==0 || operation==3) ? 2u : 0u),"Lifecycle retained invalid BoothWindow/stale approach or lost valid pair");
		}
		core::World transit("BoothWindow Transit refusal",10,4);
		transit.addRoom("Front",0,0,0,8,3); transit.addCorridor(0,3,0,8,1);
		transit.addLadder(1,0,3,{4,false,true}); transit.finishBuild(); transit.pauseSimulation(); transit.markSaved();
		auto baseline=saved(transit); std::string diagnostic;
		require(!transit.canAddBoothWindow(0,0,3,1,1,&diagnostic) && !diagnostic.empty(),"Transit BoothWindow accepted");
		bool refused=false; try { transit.addBoothWindow(0,0,3); } catch (std::exception const&) { refused=true; }
		require(refused && saved(transit)==baseline && !transit.isModified() && transit.isTraversalTopologyValid(),"Transit refusal mutated World");
	}

	void refusal(smoke::Context const&)
	{
		core::World world("BoothWindow refusal", 10, 3);
		world.addRoom("Front", 0, 0, 0, 5, 2); world.addRoom("Back", 1, 0, 0, 5, 2);
		world.addBackground(1, 0, 5, 2, 1); world.addRoom("Front background", 0, 0, 5, 2, 1);
		world.finishBuild(); world.pauseSimulation(); world.markSaved(); auto baseline = saved(world);
		for (auto p : {std::array<uint32_t,5>{1,0,1,1,1}, {0,0,8,1,1}, {0,0,5,1,1}, {0,1,1,1,1}, {0,0,1,0,1}, {0,0,1,2,1}, {0,0,1,1,2}})
		{
			std::string diagnostic; require(!world.canAddBoothWindow(p[0],p[1],p[2],p[3],p[4], &diagnostic) && !diagnostic.empty(), "Invalid placement accepted");
			bool refused = p[3]!=1 || p[4]!=1;
			if (!refused) try { world.addBoothWindow(p[0],p[1],p[2]); } catch (std::exception const&) { refused = true; }
			require(refused && saved(world) == baseline && !world.isModified() && world.isTraversalTopologyValid(), "Rejected placement mutated World");
		}
		auto booth = world.addBoothWindow(0,0,1).object; world.finishBuild(); world.markSaved(); baseline = saved(world);
		for (unsigned operation = 0; operation < 4; ++operation)
		{
			bool refused = false; try {
				if (operation == 0) booth->setState(core::Window::State::Broken);
				if (operation == 1) booth->setState(core::Window::State::Open, core::Window::Style::Tinted);
				if (operation == 2) booth->configureTraversal(true, {});
				if (operation == 3) world.createWindowTraversalResource("Forbidden", booth);
			} catch (std::exception const&) { refused = true; }
			require(refused && booth->getState() == core::Window::State::Closed && !booth->isTraversalConfigured()
				&& saved(world) == baseline && !world.isModified() && world.isTraversalTopologyValid(), "Base API leaked unsupported capabilities");
		}
	}
}
void registerBoothWindows(std::vector<smoke::Check>& checks)
{
	checks.push_back({"dumbwaiters/authoredWorld", dumbwaiters});
	checks.push_back({"dumbwaiters/wholeUnitMovement", dumbwaiterMoves});
	checks.push_back({"boothWindows/placementAndTopology", placement});
	checks.push_back({"boothWindows/atomicRefusal", refusal});
	checks.push_back({"boothWindows/lifecycle", lifecycle});
	checks.push_back({"boothWindows/simultaneousAdjacentPairs", simultaneousPairs});
}
