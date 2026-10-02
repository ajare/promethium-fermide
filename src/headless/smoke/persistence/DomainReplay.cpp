#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/AgentTagRegistry.h"
#include "core/Exceptions.h"

#include "core/Graph.h"
#include "core/World.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/SerializationException.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/Stairwell.h"
#include "core/StairwellTransit.h"
#include "core/Staircase.h"
#include "core/StaircaseTransit.h"
#include "core/YamlSerializer.h"

#include "WorldChecks.h"

namespace
{
	float controlCenterX(core::World::CreateObjectResult const& control)
	{
		auto object = control.sector->getObject(control.index)->_getObject();
		return object->getPosition().x + object->getSize().x * 0.5f;
	}

	float controlCenterY(core::World::CreateObjectResult const& control)
	{
		auto object = control.sector->getObject(control.index)->_getObject();
		return object->getPosition().y + object->getSize().y * 0.5f;
	}
}

namespace persistence
{
	using smoke::require;

	void locationEditsArePlannedAndAppliedAtomically(smoke::Context const&)
	{
		core::World world("Editable", 8, 3);
		auto room = world.addRoom("Room", 0, 0, 0, 5, 2);
		world.addSectorMarker(room, 0, 4.5f);
		auto removed = world.addSectorMarker(room, 0, 1.5f);
		world.removeSectorMarker(room, removed.index);
		world.addSectorMarker(room, 0, 2.5f);
		world.finishBuild();
		auto agent = world.createAgent("Cropped", room, 0, 4.5f);

		auto resize = world.planResizeLocation(room, 0, 0, 3, 2);
		require(resize.valid && resize.requiresConfirmation(),
			"Location shrink did not report its cascading deletions");
		world.pauseSimulation();
		auto resized = world.applyLocationEdit(resize);
		require(world.getSector(resized)->getCellsWide() == 3,
			"Location width was not changed");
		require(!world.lookupAgent(agent), "Agent cropped by resize was retained");
		uint32_t retainedObjects = 0;
		for (uint32_t i = 0; i < world.getSector(resized)->getNumObjects(); ++i)
			if (world.getSector(resized)->getObject(i)) ++retainedObjects;
		require(retainedObjects == 1, "Resize did not preserve the correct authored object slots");
		require(world.isSimulationPaused(), "Location edit resumed the simulation");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		core::World reloaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(reloaded.deserialize(*reader, workData)
			&& reloaded.getSector(resized)->getCellsWide() == 3,
			"Edited Location did not survive serialization");
		uint32_t reloadedObjects = 0;
		for (uint32_t i = 0; i < reloaded.getSector(resized)->getNumObjects(); ++i)
			if (reloaded.getSector(resized)->getObject(i)) ++reloadedObjects;
		require(reloadedObjects == 1, "Edited object tombstones did not survive serialization");

		auto movedAgent = world.createAgent("Moved", resized, 0, 1.0f);
		auto move = world.planResizeLocation(resized, 4, 0, 3, 2);
		if (!move.valid || !move.move || move.requiresConfirmation())
			throw std::runtime_error("Free Location move was not planned without deletions: "
				+ move.diagnostic + " consequences=" + std::to_string(move.consequences.size()));
		auto moved = world.applyLocationEdit(move);
		require(world.getSector(moved)->getCellX() == 4
			&& world.getSector(moved)->getCellY() == 0,
			"Location was not moved to its planned cells");
		auto movedLookup = world.lookupAgent(movedAgent);
		require(movedLookup && std::abs(movedLookup.entity->getGlobalPosition().x - 5.0f) < 0.001f
			&& std::abs(movedLookup.entity->getGlobalPosition().y) < 0.001f,
			"Agent did not move with its Location");
		std::shared_ptr<const core::SectorObject> movedObject;
		for (uint32_t i = 0; i < world.getSector(moved)->getNumObjects(); ++i)
			if (world.getSector(moved)->getObject(i)) movedObject = world.getSector(moved)->getObject(i);
		require(movedObject && movedObject->getCellX() == 6,
			"Sector object did not move with its Location");

		auto remove = world.planRemoveLocation(moved);
		require(remove.valid, "Valid Location deletion was rejected");
		world.applyLocationEdit(remove);
		require(world.getNumSectors() == 0, "Deleted Location was retained");

		core::World fullWidthWorld("Full width", 16, 2);
		auto createdFullWidth = fullWidthWorld.addCorridor(0, 0, 16);
		require(fullWidthWorld.getSector(createdFullWidth)->getCellX1() == 15,
			"Location creation did not include the final world column");

		core::World boundaryWorld("Boundary", 16, 2);
		auto boundaryCorridor = boundaryWorld.addCorridor(0, 0, 15);
		boundaryWorld.finishBuild();
		auto boundaryResize = boundaryWorld.planResizeLocation(boundaryCorridor, 0, 0, 16, 1);
		require(boundaryResize.valid,
			"Location could not be resized through the final world column");
		boundaryWorld.pauseSimulation();
		auto fullWidthCorridor = boundaryWorld.applyLocationEdit(boundaryResize);
		require(boundaryWorld.getSector(fullWidthCorridor)->getCellX1() == 15,
			"Location resize did not include the final world column");
	}

	void structuralReplayPreservesIndividualAgentPropertiesAndRuntimeState(smoke::Context const&)
	{
		// Per-Agent samples are authored by assigning a tag that carries sampled
		// properties; every property type rides on one tag so the carried Agent
		// must keep all thirteen of its persisted draws.
		auto registry = core::AgentTagRegistry::create();
		auto const sampledTag = registry->addAgentTag("sampled");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(sampledTag, &diagnostic)
			&& registry->setAgentTagWalkSpeedModifier(sampledTag, { 1.1f, 1.2f }, &diagnostic)
			&& registry->addAgentTagHeightModifier(sampledTag, &diagnostic)
			&& registry->setAgentTagHeightModifier(sampledTag, { 0.9f, 1.0f }, &diagnostic)
			&& registry->addAgentTagStairSpeedModifier(sampledTag, &diagnostic)
			&& registry->setAgentTagStairSpeedModifier(sampledTag, { 0.8f, 0.9f }, &diagnostic)
			&& registry->addAgentTagLadderSpeedModifier(sampledTag, &diagnostic)
			&& registry->setAgentTagLadderSpeedModifier(sampledTag, { 0.9f, 1.0f }, &diagnostic)
			&& registry->addAgentTagInteractionAversion(sampledTag, &diagnostic)
			&& registry->setAgentTagInteractionAversion(sampledTag, { 0.1f, 0.2f }, &diagnostic)
			&& registry->addAgentTagEffortAversion(sampledTag, &diagnostic)
			&& registry->setAgentTagEffortAversion(sampledTag, { 0.2f, 0.3f }, &diagnostic)
			&& registry->addAgentTagWaitingAversion(sampledTag, &diagnostic)
			&& registry->setAgentTagWaitingAversion(sampledTag, { 0.5f, 0.6f }, &diagnostic)
			&& registry->addAgentTagCrowdAversion(sampledTag, &diagnostic)
			&& registry->setAgentTagCrowdAversion(sampledTag, { 0.4f, 0.5f }, &diagnostic)
			&& registry->addAgentTagRiskAversion(sampledTag, &diagnostic)
			&& registry->setAgentTagRiskAversion(sampledTag, { 0.5f, 0.6f }, &diagnostic)
			&& registry->addAgentTagRouteFamiliarity(sampledTag, &diagnostic)
			&& registry->setAgentTagRouteFamiliarity(sampledTag, { 0.6f, 0.7f }, &diagnostic)
			&& registry->addAgentTagRoutePersistence(sampledTag, &diagnostic)
			&& registry->setAgentTagRoutePersistence(sampledTag, { 0.7f, 0.8f }, &diagnostic)
			&& registry->addAgentTagMinimumRoutePlanningTime(sampledTag, &diagnostic)
			&& registry->setAgentTagMinimumRoutePlanningTime(sampledTag, { 2.0f, 2.5f }, &diagnostic)
			&& registry->addAgentTagMaximumRoutePlanningTime(sampledTag, &diagnostic)
			&& registry->setAgentTagMaximumRoutePlanningTime(sampledTag, { 4.0f, 4.5f }, &diagnostic),
			"The fixture could not author its sampled properties: " + diagnostic);

		core::World world("Replayed", 16, 3);
		world.attachAgentTagRegistry("replay.tags.yaml", registry);
		auto const home = world.addRoom("Home", 0, 0, 0, 6, 2);
		auto const other = world.addRoom("Other", 0, 0, 8, 6, 2);
		uint32_t destinationIdentifier{ 0x5245504cu };
		uint32_t secondDestinationIdentifier{ 0x5245504du };
		world.addSectorMarker(home, 0, 4.5f, &destinationIdentifier);
		world.addSectorMarker(home, 0, 2.5f, &secondDestinationIdentifier);
		world.finishBuild();
		auto const agentId = world.createAgent("Individual", home, 0, 1.0f);
		world.pauseSimulation();
		require(world.assignAgentTag(agentId, sampledTag, &diagnostic),
			"The fixture could not assign its Agent tag: " + diagnostic);
		auto* agent = world.lookupAgent(agentId).entity;

		core::MobilityProfile mobility;
		mobility.set(core::TraversalKind::Ladder, core::MobilityUse::CannotUse);
		require(world.setAgentIndividualColour(agentId, core::AgentColour{ 12, 34, 56 }, &diagnostic)
			&& world.setAgentIndividualEscalatorWalkingChance(agentId, 0.25f, &diagnostic)
			&& world.setAgentIndividualWalkSpeedModifier(agentId, 1.15f, &diagnostic)
			&& world.setAgentIndividualHeightModifier(agentId, 0.95f, &diagnostic)
			&& world.setAgentIndividualStairSpeedModifier(agentId, 0.8f, &diagnostic)
			&& world.setAgentIndividualLadderSpeedModifier(agentId, 0.9f, &diagnostic)
			&& world.setAgentIndividualInteractionAversion(agentId, 0.15f, &diagnostic)
			&& world.setAgentIndividualEffortAversion(agentId, 0.25f, &diagnostic)
			&& world.setAgentIndividualWaitingAversion(agentId, 0.55f, &diagnostic)
			&& world.setAgentIndividualCrowdAversion(agentId, 0.45f, &diagnostic)
			&& world.setAgentIndividualRiskAversion(agentId, 0.55f, &diagnostic)
			&& world.setAgentIndividualRouteFamiliarity(agentId, 0.65f, &diagnostic)
			&& world.setAgentIndividualRoutePersistence(agentId, 0.75f, &diagnostic)
			&& world.setAgentIndividualMinimumRoutePlanningTime(agentId, 2.5f, &diagnostic)
			&& world.setAgentIndividualMaximumRoutePlanningTime(agentId, 4.5f, &diagnostic)
			&& world.setAgentIndividualPermissionAdherence(agentId, false, &diagnostic)
			&& world.setAgentIndividualMobilityProfile(agentId, mobility, &diagnostic),
			"Individual Agent properties were not authored: " + diagnostic);

		// Runtime grants made before the edit are pause/resume state and survive
		// the replay; authored grants stay empty so the current grants afterwards
		// prove the runtime overlay itself travelled (#328).
		auto const permission = world.addAccessPermission("Night access");
		require(world.setAgentRuntimeAccessPermissionGrant(agentId, permission, true),
			"Runtime Access grant was not applied");
		auto const permissionSet = world.addPermissionSet("Night set");
		require(world.setAgentRuntimePermissionSetAssignment(agentId, permissionSet, true),
			"Runtime Permission set assignment was not applied");

		// Two assignments to different destinations always advance the journey
		// stream (Vertex ids are process-global, so the first alone might not),
		// giving the replay a non-zero stream position it must preserve.
		auto destination = world.getGraph()->getVertexByIdentifier(destinationIdentifier);
		auto path = world.getGraph()->calculatePath(agent, destination);
		require(path && !path->nodes.empty(), "Agent path could not be created");
		agent->setPath(std::move(path), false);
		auto secondDestination = world.getGraph()->getVertexByIdentifier(secondDestinationIdentifier);
		auto secondPath = world.getGraph()->calculatePath(agent, secondDestination);
		require(secondPath && !secondPath->nodes.empty(), "Second Agent path could not be created");
		agent->setPath(std::move(secondPath), false);
		auto const journeyBefore = agent->getRouteJourneyIdentity(nullptr);
		require(journeyBefore != 0, "Route journey stream did not advance across two path assignments");

		// Snapshot everything the replay must preserve; the persisted samples are
		// deterministic draws, so they are compared against themselves across it.
		auto const colourBefore = agent->getIndividualColour();
		auto const escalatorChanceBefore = agent->getIndividualEscalatorWalkingChance();
		auto const walkSpeedBefore = agent->getIndividualWalkSpeedModifier();
		auto const heightBefore = agent->getIndividualHeightModifier();
		auto const stairSpeedBefore = agent->getIndividualStairSpeedModifier();
		auto const ladderSpeedBefore = agent->getIndividualLadderSpeedModifier();
		auto const interactionBefore = agent->getIndividualInteractionAversion();
		auto const effortBefore = agent->getIndividualEffortAversion();
		auto const waitingBefore = agent->getIndividualWaitingAversion();
		auto const crowdBefore = agent->getIndividualCrowdAversion();
		auto const riskBefore = agent->getIndividualRiskAversion();
		auto const familiarityBefore = agent->getIndividualRouteFamiliarity();
		auto const persistenceBefore = agent->getIndividualRoutePersistence();
		auto const minimumPlanningBefore = agent->getIndividualMinimumRoutePlanningTime();
		auto const maximumPlanningBefore = agent->getIndividualMaximumRoutePlanningTime();
		auto const adherenceBefore = agent->getIndividualPermissionAdherence();
		auto const mobilityBefore = agent->getIndividualMobilityProfile();
		auto const walkSampleBefore = agent->getWalkSpeedModifierSample();
		auto const heightSampleBefore = agent->getHeightModifierSample();
		auto const stairSampleBefore = agent->getStairSpeedModifierSample();
		auto const ladderSampleBefore = agent->getLadderSpeedModifierSample();
		auto const interactionSampleBefore = agent->getInteractionAversionSample();
		auto const effortSampleBefore = agent->getEffortAversionSample();
		auto const waitingSampleBefore = agent->getWaitingAversionSample();
		auto const crowdSampleBefore = agent->getCrowdAversionSample();
		auto const riskSampleBefore = agent->getRiskAversionSample();
		auto const familiaritySampleBefore = agent->getRouteFamiliaritySample();
		auto const persistenceSampleBefore = agent->getRoutePersistenceSample();
		auto const minimumPlanningSampleBefore = agent->getMinimumRoutePlanningTimeSample();
		auto const maximumPlanningSampleBefore = agent->getMaximumRoutePlanningTimeSample();

		// An edit to a Room the Agent does not even stand in replays the World
		// and must change nothing about the carried Agent.
		auto resize = world.planResizeLocation(other, 8, 0, 5, 2);
		if (!resize.valid || resize.requiresConfirmation())
			throw std::runtime_error("Unrelated Location shrink was not planned cleanly: "
				+ resize.diagnostic);
		auto const shrunk = world.applyLocationEdit(resize);
		require(world.getSector(shrunk)->getCellsWide() == 5,
			"Unrelated Location width was not changed");

		auto const carried = world.lookupAgent(agentId);
		require(carried && std::abs(carried.entity->getGlobalPosition().x - 1.0f) < 0.0001f,
			"Agent was dropped or moved by an unrelated Location edit");
		auto const* kept = carried.entity;

		require(kept->getIndividualColour() == colourBefore
				&& kept->getIndividualEscalatorWalkingChance() == escalatorChanceBefore
				&& kept->getIndividualWalkSpeedModifier() == walkSpeedBefore
				&& kept->getIndividualHeightModifier() == heightBefore
				&& kept->getIndividualStairSpeedModifier() == stairSpeedBefore
				&& kept->getIndividualLadderSpeedModifier() == ladderSpeedBefore
				&& kept->getIndividualInteractionAversion() == interactionBefore
				&& kept->getIndividualEffortAversion() == effortBefore
				&& kept->getIndividualWaitingAversion() == waitingBefore
				&& kept->getIndividualCrowdAversion() == crowdBefore
				&& kept->getIndividualRiskAversion() == riskBefore
				&& kept->getIndividualRouteFamiliarity() == familiarityBefore
			&& kept->getIndividualRoutePersistence() == persistenceBefore
				&& kept->getIndividualMinimumRoutePlanningTime() == minimumPlanningBefore
				&& kept->getIndividualMaximumRoutePlanningTime() == maximumPlanningBefore
				&& kept->getIndividualPermissionAdherence() == adherenceBefore
				&& kept->getIndividualMobilityProfile() == mobilityBefore,
			"Individual Agent properties were lost across the structural replay");
		require(kept->getIndividualPermissionAdherence()
				&& !*kept->getIndividualPermissionAdherence(),
			"Authored Permission adherence false reverted to the default across the replay");
		require(kept->getIndividualMobilityProfile()
				&& kept->getIndividualMobilityProfile()->get(core::TraversalKind::Ladder)
					== core::MobilityUse::CannotUse,
			"Individual Mobility profile was lost across the structural replay");
		require(kept->getWalkSpeedModifierSample() == walkSampleBefore
				&& kept->getHeightModifierSample() == heightSampleBefore
				&& kept->getStairSpeedModifierSample() == stairSampleBefore
				&& kept->getLadderSpeedModifierSample() == ladderSampleBefore
				&& kept->getInteractionAversionSample() == interactionSampleBefore
				&& kept->getEffortAversionSample() == effortSampleBefore
				&& kept->getWaitingAversionSample() == waitingSampleBefore
				&& kept->getCrowdAversionSample() == crowdSampleBefore
				&& kept->getRiskAversionSample() == riskSampleBefore
				&& kept->getRouteFamiliaritySample() == familiaritySampleBefore
				&& kept->getRoutePersistenceSample() == persistenceSampleBefore
				&& kept->getMinimumRoutePlanningTimeSample() == minimumPlanningSampleBefore
				&& kept->getMaximumRoutePlanningTimeSample() == maximumPlanningSampleBefore,
			"Persisted tag samples were lost across the structural replay");
		require(kept->getWalkSpeedModifierSample() && kept->getInteractionAversionSample()
				&& kept->getEffortAversionSample() && kept->getWaitingAversionSample()
				&& kept->getCrowdAversionSample() && kept->getRiskAversionSample()
				&& kept->getRouteFamiliaritySample() && kept->getRoutePersistenceSample(),
			"The fixture left Agents without their persisted samples");

		auto currentGrants = world.getAgentCurrentDirectAccessGrants(agentId);
		require(find(currentGrants.begin(), currentGrants.end(), permission) != currentGrants.end(),
			"Runtime Access grant overlay was lost across the structural replay");
		auto currentSets = world.getAgentCurrentPermissionSetAssignments(agentId);
		require(find(currentSets.begin(), currentSets.end(), permissionSet) != currentSets.end(),
			"Runtime Permission set overlay was lost across the structural replay");
		require(kept->getRouteJourneyIdentity(nullptr) == journeyBefore,
			"Route journey stream position diverged from the uninterrupted run");
	}

	void editedShuttleRoundTripsWithoutSchemaChanges(smoke::Context const&)
	{
		core::World world("Serializable Shuttle", 32, 3);
		world.addCorridor(0, 0, 31);
		world.addCorridor(1, 0, 31);
		core::World::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101;
		options.minimumDwellSeconds = 1.25f;
		options.maximumBoardingSeconds = 4.5f;
		auto created = world.addShuttle(1, 0, 0, 27, options);
		world.finishBuild();
		world.pauseSimulation();
		auto move = world.planResizeShuttle(created.shuttle.sector->getIndex(), 1, 1, 27);
		require(move.valid, "Serializable Shuttle move was rejected");
		auto shuttleIndex = world.applyShuttleEdit(move);
		auto add = world.planAddShuttleStop(shuttleIndex, 9);
		require(add.valid, "Serializable Shuttle stop addition was rejected");
		shuttleIndex = world.applyShuttleEdit(add);

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		auto yaml = writer->getSerializedString();
		require(yaml.find("type: shuttle") != std::string::npos
			&& yaml.find("numCars: 2") != std::string::npos
			&& yaml.find("capacityPerCarriage: 2") != std::string::npos
			&& yaml.find("doorMask: 5") != std::string::npos
			&& yaml.find("allowPartialLandings: false") != std::string::npos,
			"Edited Shuttle did not use the existing explicit YAML schema");

		core::World loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "Edited Shuttle YAML did not deserialize");
		auto shuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(loaded.getSector(shuttleIndex));
		require(shuttle && shuttle->getCellX() == 1 && shuttle->getCellY() == 1
			&& shuttle->getCellsWide() == 27 && shuttle->getNumStops() == 3,
			"Edited Shuttle geometry or stops did not round-trip");
		core::World::CreateShuttleOptions loadedOptions{};
		require(loaded.getShuttleOptions(shuttle->getShuttle().get(), loadedOptions)
			&& loadedOptions.numCars == 2 && loadedOptions.carWidth == 3
			&& loadedOptions.capacity == 2 && loadedOptions.doorMask == 0b101
			&& std::abs(loadedOptions.minimumDwellSeconds - 1.25f) < 0.001f
			&& std::abs(loadedOptions.maximumBoardingSeconds - 4.5f) < 0.001f
			&& !loadedOptions.allowPartialLandings,
			"Edited Shuttle configuration did not round-trip");
	}

	void physicalControlsPreferDistinctWallPositions(smoke::Context const&)
	{
		core::World world("Control placement", 9, 2);
		world.addCorridor(0, 1, 7);
		auto room = world.addRoom("Back room", 1, 0, 4, 3, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[1] = true;

		auto first = world.addSectorDoor(0, 0, 5, options);
		auto second = world.addSectorDoor(0, 0, 6, options);
		require(std::abs(controlCenterX(first.controls[1]) - 5.0f) < 0.0001f
			&& std::abs(controlCenterX(second.controls[1]) - 6.0f) < 0.0001f,
			"Adjacent Citadel-style Door controls did not choose distinct X positions");
		auto standardY = CORE_BUTTON_Y_OFFSET
			+ first.controls[1].sector->getObject(first.controls[1].index)
				->_getObject()->getSize().y * 0.5f;
		require(std::abs(controlCenterY(first.controls[1]) - standardY) < 0.0001f
			&& std::abs(controlCenterY(second.controls[1]) - standardY) < 0.0001f,
			"Separated Door controls retained obsolete height offsets");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		core::World replayed("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(replayed.deserialize(*reader, workData), "Control-placement replay failed");
		std::vector<float> replayedCenters;
		for (uint32_t i = 0; i < replayed.getSector(room)->getNumObjects(); ++i)
		{
			auto object = replayed.getSector(room)->getObject(i);
			if (object && object->getObjectType() == core::SectorObjectType::InteractionPoint)
				replayedCenters.push_back(object->_getObject()->getPosition().x
					+ object->_getObject()->getSize().x * 0.5f);
		}
		std::sort(replayedCenters.begin(), replayedCenters.end());
		require(replayedCenters.size() == 2
			&& std::abs(replayedCenters[0] - 5.0f) < 0.0001f
			&& std::abs(replayedCenters[1] - 6.0f) < 0.0001f,
			"Control placement was not deterministic after YAML replay");

		world.finishBuild();
		world.pauseSimulation();
		require(world.removeSectorDoor(second.door.sector->getIndex(), second.door.index),
			"Adjacent Door could not be removed");
		std::vector<float> remainingCenters;
		for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
		{
			auto object = world.getSector(room)->getObject(i);
			if (object && object->getObjectType() == core::SectorObjectType::InteractionPoint)
				remainingCenters.push_back(object->_getObject()->getPosition().x
					+ object->_getObject()->getSize().x * 0.5f);
		}
		require(remainingCenters.size() == 1
			&& std::abs(remainingCenters[0] - 6.0f) < 0.0001f,
			"Remaining Door control did not return to its preferred position after removal");

		core::World fallback("Control fallback", 4, 2);
		fallback.addCorridor(0, 0, 2);
		fallback.addRoom("Narrow back room", 1, 0, 0, 2, 1);
		auto left = fallback.addSectorDoor(0, 0, 0, options);
		auto right = fallback.addSectorDoor(0, 0, 1, options);
		require(std::abs(controlCenterX(left.controls[1])
				- controlCenterX(right.controls[1])) < 0.0001f
			&& std::abs(controlCenterY(left.controls[1])
				- controlCenterY(right.controls[1])) > 0.049f,
			"Unavoidable same-X controls did not use the height fallback");
	}

	void platformLiftStopDurationRoundTrips(smoke::Context const&)
	{
		core::World original("Serializable PlatformLift", 7, 4);
		auto room = original.addRoom("Platform room", 0, 0, 0, 6, 3);
		for (uint32_t level = 1; level <= 2; ++level)
		{
			original.addSectorWalkway(room, level, 2);
			original.addSectorWalkway(room, level, 3);
		}
		core::World::CreateLiftOptions options;
		options.stopOffsets = { 0, 1, 2 };
		options.platformStopDurationSeconds = 3.5f;
		auto created = original.addSectorPlatformLift(room, 0, 2, options);
		original.finishBuild();

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto yaml = writer->getSerializedString();
		require(yaml.find("stopDurationSeconds: 3.5") != std::string::npos
			&& yaml.find("minimumDwellSeconds") == std::string::npos
			&& yaml.find("maximumBoardingSeconds") == std::string::npos,
			"PlatformLift did not serialize its single stop timer");

		core::World loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "PlatformLift YAML did not deserialize");
		core::World::CreateLiftOptions loadedOptions;
		require(loaded.getPlatformLiftOptions(created.lift.sector->getIndex(), created.lift.index,
				loadedOptions)
			&& std::abs(loadedOptions.platformStopDurationSeconds - 3.5f) < 0.001f,
			"PlatformLift stop timer did not round-trip");

		core::World::CreateLiftOptions defaults;
		require(std::abs(defaults.platformStopDurationSeconds
			- CORE_PLATFORM_LIFT_STOP_DURATION) < 0.001f,
			"PlatformLift stop timer default is not the Defines.h value");
	}

	void enclosedLiftsSupportMultiLevelRooms(smoke::Context const&)
	{
		core::World world("Room lift", 16, 3);
		auto room = world.addRoom("Lift Hall", 0, 0, 0, 16, 3);
		for (uint32_t level = 1; level < 3; ++level)
			for (uint32_t x = 0; x < 16; ++x)
				world.addSectorWalkway(room, level, x);

		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.levelsHigh = 3;
		options.stopOffsets = { 0, 1, 2 };
		auto created = world.addLift(1, 0, 8, options);
		world.addSectorMarker(room, 1, 0.5f);
		world.addSectorMarker(room, 2, 15.5f);
		world.finishBuild();

		auto lift = std::dynamic_pointer_cast<const core::LiftTransit>(created.lift.sector);
		require(lift && lift->getNumStops() == 3 && created.doors.size() == 3,
			"An enclosed Lift could not connect Ground and Walkways in one Fore-layer Room");
	}

	void stopDerivingAddLiftRejectsInvalidLayerIndex(smoke::Context const&)
	{
		core::World world("Lift layer validation", 8, 2);

		bool rejected = false;
		try
		{
			// Layer 0 is the front-most Layer: it has no Layer in front for the landings.
			world.addLift(0, 0, 2, 1, 1);
		}
		catch (core::WorldException const&)
		{
			rejected = true;
		}
		require(rejected, "The stop-deriving addLift() did not reject the front-most Layer");

		rejected = false;
		try
		{
			world.addLift(2, 0, 2, 1, 1);
		}
		catch (core::WorldException const&)
		{
			rejected = true;
		}
		require(rejected, "The stop-deriving addLift() did not reject a Layer past the layer count");
	}

	void stairwellSectorsAreCanvasSelectable(smoke::Context const&)
	{
		core::Stairwell leftStairwell(0, 0, 3, CORE_SIDE_LEFT);
		core::Stairwell rightStairwell(0, 0, 3, CORE_SIDE_RIGHT);
		auto left = leftStairwell.getLevelPath(0);
		auto right = rightStairwell.getLevelPath(0);
		for (size_t i = 0; i < left.size(); ++i)
			require(std::abs(left[i].x + right[i].x - 2.0f) < 0.0001f
				&& left[i].y == right[i].y,
				"Right-mounted Stairwell path is not mirrored horizontally");
		require(left[0].x == 1.0f && left[0].y == 0.0f
			&& std::abs(left[1].x - 1.666f) < 0.0001f && left[1].y == 0.25f
			&& std::abs(left[2].x - 0.334f) < 0.0001f && left[2].y == 0.75f
			&& left[3].x == 1.0f && left[3].y == 1.0f,
			"Stairwell primitive endpoints do not match its path vertices");
		auto nextLevel = leftStairwell.getLevelPath(1);
		require(left[3] == nextLevel[0],
			"Adjacent Stairwell diagonal paths do not share a level endpoint");
	}

	void staircasesConnectAdjacentCorridorsAndRoundTrip(smoke::Context const&)
	{
		core::Staircase right(0, 0, 4, CORE_SIDE_RIGHT);
		core::Staircase left(0, 0, 4, CORE_SIDE_LEFT);
		auto rightPath = right.getPath();
		auto leftPath = left.getPath();
		require(right.getStepCount() == 32 && rightPath[0].x == 0.0f
			&& rightPath[1].x == 4.0f && leftPath[0].x == 4.0f
			&& leftPath[1].x == 0.0f,
			"Staircase direction, endpoints, or width-based step count is incorrect");
		float const pathLength = rightPath[0].distanceTo(rightPath[1]);
		core::Staircase upEscalator(0, 0, 4, CORE_SIDE_RIGHT, 1.0f);
		core::Staircase downEscalator(0, 0, 4, CORE_SIDE_RIGHT, -1.0f);
		upEscalator.update(pathLength * 0.25f);
		downEscalator.update(pathLength * 0.25f);
		right.update(pathLength);
		require(std::abs(upEscalator.getAnimationPhase() - 0.25f) < 0.001f
			&& std::abs(downEscalator.getAnimationPhase() - 0.75f) < 0.001f
			&& right.getAnimationPhase() == 0.0f,
			"Escalator step animation does not follow its signed world speed");

		core::World world("Staircase", 6, 3);
		world.addCorridor(0, 0, 1);
		world.addCorridor(0, 3, 1);
		world.addCorridor(1, 0, 1);
		world.addCorridor(1, 3, 1);
		std::string diagnostic;
		require(!world.canAddStaircase(1, 0, 0, 1, CORE_SIDE_RIGHT, &diagnostic),
			"A one-cell Staircase was accepted");
		require(world.canAddStaircase(1, 0, 0, 4, CORE_SIDE_RIGHT, &diagnostic),
			"A valid Staircase between endpoint Corridors was rejected");
		auto index = world.addStaircase(1, 0, 0, 4, CORE_SIDE_RIGHT, 1.25f);
		world.finishBuild();
		auto transit = std::dynamic_pointer_cast<const core::StaircaseTransit>(world.getSector(index));
		require(transit && transit->getCellsWide() == 4 && transit->getLevelsHigh() == 2,
			"Staircase Transit has the wrong footprint");
		core::World::CreateStaircaseOptions options;
		require(world.getStaircaseOptions(index, options) && options.cellsWide == 4
			&& options.riseSide == CORE_SIDE_RIGHT && std::abs(options.speed - 1.25f) < 0.001f,
			"Staircase authored options were not retained");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData); writer->serialize();
		auto yaml = writer->getSerializedString();
		require(yaml.find("type: staircase") != std::string::npos
			&& yaml.find("speed: 1.25") != std::string::npos,
			"Staircase speed was not serialized");
		core::World loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml); reader->deserialize();
		require(loaded.deserialize(*reader, workData), "Staircase YAML did not deserialize");
		auto loadedTransit = std::dynamic_pointer_cast<const core::StaircaseTransit>(loaded.getSector(index));
		require(loadedTransit && loadedTransit->getRiseSide() == CORE_SIDE_RIGHT
			&& std::abs(loadedTransit->getStaircase()->getSpeed() - 1.25f) < 0.001f,
			"Staircase did not round-trip through YAML");

		auto escalatorEdge = std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [](auto const& edge)
			{ return edge->getType() == core::EdgeType::Staircase; });
		require(escalatorEdge != world.getGraph()->getEdges().end(),
			"Escalator traversal edge was not created");
		auto edge = *escalatorEdge;
		auto low = edge->getVertex(0)->getPosition().y < edge->getVertex(1)->getPosition().y
			? edge->getVertex(0) : edge->getVertex(1);
		auto high = low == edge->getVertex(0) ? edge->getVertex(1) : edge->getVertex(0);
		auto const& routePolicy = world.getRouteChoicePolicy();
		core::RouteDecisionContext const routeContext{ nullptr, routePolicy.baselineProfile,
			routePolicy, nullptr, CORE_AGENT_BASE_WALK_SPEED, &world,
			CORE_AGENT_BASE_CLIMB_SPEED };
		auto const upwardFacts = edge->getDirectedTraversalFacts(high, routeContext);
		auto const downwardFacts = edge->getDirectedTraversalFacts(low, routeContext);
		require(edge->isTraversable(high, nullptr) && !edge->isTraversable(low, nullptr)
			&& upwardFacts.feasible && !downwardFacts.feasible
			&& downwardFacts.exclusionReason == core::RouteExclusionReason::Direction
			&& std::abs(edge->getTraversalSpeed(nullptr) - 1.25f) < 0.001f,
			"Positive-speed Escalator is not one-way upward at its configured speed");

		world.pauseSimulation();
		auto flip = world.planResizeStaircase(index, 0, 0, { 4, CORE_SIDE_LEFT, -0.75f });
		require(flip.valid, "A valid Staircase direction flip was rejected");
		index = world.applyStaircaseEdit(flip);
		transit = std::dynamic_pointer_cast<const core::StaircaseTransit>(world.getSector(index));
		require(transit && transit->getRiseSide() == CORE_SIDE_LEFT
			&& std::abs(transit->getStaircase()->getSpeed() + 0.75f) < 0.001f,
			"Staircase direction or Escalator speed was not edited");
		escalatorEdge = std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [](auto const& candidate)
			{ return candidate->getType() == core::EdgeType::Staircase; });
		edge = *escalatorEdge;
		low = edge->getVertex(0)->getPosition().y < edge->getVertex(1)->getPosition().y
			? edge->getVertex(0) : edge->getVertex(1);
		high = low == edge->getVertex(0) ? edge->getVertex(1) : edge->getVertex(0);
		require(edge->isTraversable(low, nullptr) && !edge->isTraversable(high, nullptr)
			&& std::abs(edge->getTraversalSpeed(nullptr) - 0.75f) < 0.001f,
			"Negative-speed Escalator is not one-way downward at its configured speed");
		auto removal = world.planRemoveStaircase(index);
		require(removal.valid && removal.requiresConfirmation(),
			"Staircase deletion was not planned as a confirmed edit");
		require(world.applyStaircaseEdit(removal) == ~0u,
			"Staircase deletion did not return the removed-sector sentinel");
		require(!static_cast<core::World const&>(world).getLayer(1)
			->getCellDefinition(0, 0).occupied(),
			"Deleted Staircase still occupies the Back layer");
	}

	void laddersCanBeValidatedEditedAndDeleted(smoke::Context const&)
	{
		core::World edgeWorld("Edge Ladder controls", 5, 3);
		edgeWorld.addCorridor(0, 0, 5);
		edgeWorld.addCorridor(2, 0, 5);
		auto edgeLadder = edgeWorld.addLadder(1, 0, 4, { 3, true, true });
		auto interiorLadder = edgeWorld.addLadder(1, 0, 0, { 3, true, true });
		auto retractedLadder = edgeWorld.addLadder(1, 0, 2, { 3, true, false });
		core::Vector2 retractedMin, retractedMax;
		std::static_pointer_cast<const core::LadderTransit>(retractedLadder.ladder.sector)
			->getLadder()->getCurrentShape(retractedMin, retractedMax);
		require(std::abs((retractedMax.y - retractedMin.y) - 0.2f) < 0.0001f,
			"Retracted Ladders were not rendered at the minimum 0.2 length");
		require(std::abs(controlCenterX(edgeLadder.controls[CORE_LADDER_ENDPOINT_LOW]) - 4.2f) < 0.0001f
			&& std::abs(controlCenterX(edgeLadder.controls[CORE_LADDER_ENDPOINT_HIGH]) - 4.2f) < 0.0001f,
			"Left-side Ladder controls were not placed at the cell's 0.2 offset");
		require(std::abs(controlCenterX(interiorLadder.controls[CORE_LADDER_ENDPOINT_LOW]) - 0.8f) < 0.0001f
			&& std::abs(controlCenterX(interiorLadder.controls[CORE_LADDER_ENDPOINT_HIGH]) - 0.8f) < 0.0001f,
			"Right-side Ladder controls were not placed at the cell's 0.8 offset");

		core::World world("Ladder editing", 10, 5);
		std::vector<uint32_t> corridors;
		for (uint32_t y = 0; y < 5; ++y) corridors.push_back(world.addCorridor(y, 0, 10));
		std::string diagnostic;
		require(!world.canAddLadder(1, 0, 1, 1, &diagnostic)
			&& diagnostic.find("at least two") != std::string::npos,
			"Ladder placement accepted a one-level footprint");
		require(world.canAddLadder(1, 0, 1, 3, &diagnostic),
			"Valid Ladder placement was rejected");
		auto created = world.addLadder(1, 0, 1, { 3, false, true });
		world.finishBuild();
		world.pauseSimulation();
		auto agentId = world.createAgent("Ladder user", created.ladder.sector->getIndex(), 1, 0.5f);
		auto originalAgentPosition = world.lookupAgent(agentId).entity->getGlobalPosition();

		core::World::CreateLadderOptions edited{ 3, true, false, 3 };
		auto move = world.planResizeLadder(created.ladder.sector->getIndex(), 4, 1, edited);
		require(move.valid && move.move, "Valid Ladder move was not planned");
		auto movedIndex = world.applyLadderEdit(move);
		auto ladder = std::dynamic_pointer_cast<const core::LadderTransit>(world.getSector(movedIndex));
		require(ladder && ladder->getCellX() == 4 && ladder->getCellY() == 1
			&& ladder->getLevelsHigh() == 3,
			"Ladder geometry was not edited");
		auto movedAgent = world.lookupAgent(agentId).entity;
		require(movedAgent
			&& std::abs(movedAgent->getGlobalPosition().x - originalAgentPosition.x - 3.0f) < 0.001f
			&& std::abs(movedAgent->getGlobalPosition().y - originalAgentPosition.y - 1.0f) < 0.001f,
			"An occupying Agent did not move with the Ladder");
		core::World::CreateLadderOptions loaded{};
		require(world.getLadderOptions(movedIndex, loaded) && loaded.extensible
			&& !loaded.startExtended && loaded.directionalBatchLimit == 3,
			"Ladder configuration was not retained");
		auto countControls = [&](uint32_t sectorIndex)
		{
			uint32_t count = 0;
			auto sector = world.getSector(sectorIndex);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (sector->getObject(i)->getObjectType() == core::SectorObjectType::InteractionPoint)
					++count;
			return count;
		};
		require(countControls(corridors[1]) == 1 && countControls(corridors[3]) == 1,
			"Enabling Ladder extensibility did not create both endpoint controls");

		auto unsupportedLocation = world.planResizeLocation(corridors[1], 0, 1, 3, 1);
		require(!unsupportedLocation.valid
			&& unsupportedLocation.diagnostic.find("Ladder") != std::string::npos,
			"A Location edit was allowed to invalidate a Ladder endpoint");
		auto blocked = world.planResizeLadder(movedIndex, 10, 1, edited);
		require(!blocked.valid, "Out-of-bounds Ladder edit was accepted");
		auto removal = world.planRemoveLadder(movedIndex);
		require(removal.valid && removal.requiresConfirmation(),
			"Ladder deletion was not planned as a confirmed edit");
		require(world.applyLadderEdit(removal) == ~0u,
			"Ladder deletion did not return the removed-sector sentinel");
		require(!static_cast<core::World const&>(world).getLayer(1)
			->getCellDefinition(4, 1).occupied(),
			"Deleted Ladder still occupies the Back layer");
	}

	void stairwellsCanBeValidatedEditedAndDeleted(smoke::Context const&)
	{
		core::World world("Stairwell editing", 10, 5);
		for (uint32_t y = 0; y < 5; ++y) world.addCorridor(y, 0, 10);
		std::string diagnostic;
		require(!world.canAddStairwell(1, 0, 1, 1, &diagnostic)
			&& diagnostic.find("at least two") != std::string::npos,
			"Stairwell placement accepted a one-level footprint");
		require(world.canAddStairwell(1, 0, 1, 3, &diagnostic),
			"Valid Stairwell placement was rejected");
		auto created = world.addStairwell(1, 0, 1,
			core::World::CreateStairwellOptions{ 3, CORE_SIDE_LEFT });
		world.finishBuild();
		world.pauseSimulation();
		auto agentId = world.createAgent("Stair user", created.sectorIndex, 1, 1.0f);
		auto originalAgentPosition = world.lookupAgent(agentId).entity->getGlobalPosition();

		core::World::CreateStairwellOptions edited{ 3, CORE_SIDE_RIGHT, 2, 3 };
		auto move = world.planResizeStairwell(created.sectorIndex, 4, 1, edited);
		require(move.valid && move.move, "Valid Stairwell move was not planned");
		auto movedIndex = world.applyStairwellEdit(move);
		auto stairwell = std::dynamic_pointer_cast<const core::StairwellTransit>(
			world.getSector(movedIndex));
		require(stairwell && stairwell->getCellX() == 4 && stairwell->getCellY() == 1
			&& stairwell->getLevelsHigh() == 3 && stairwell->getMountSide() == CORE_SIDE_RIGHT,
			"Stairwell geometry or mounting side was not edited");
		auto movedAgent = world.lookupAgent(agentId).entity;
		require(movedAgent
			&& std::abs(movedAgent->getGlobalPosition().x - originalAgentPosition.x - 3.0f) < 0.001f
			&& std::abs(movedAgent->getGlobalPosition().y - originalAgentPosition.y - 1.0f) < 0.001f,
			"An occupying Agent did not move with the Stairwell");
		core::World::CreateStairwellOptions loaded{};
		require(world.getStairwellOptions(movedIndex, loaded)
			&& loaded.directionalCapacity == 2 && loaded.directionalBatchLimit == 3,
			"Stairwell coordination properties were not retained");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		core::World replayed("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(replayed.deserialize(*reader, workData), "Edited Stairwell YAML did not deserialize");
		core::World::CreateStairwellOptions replayedOptions{};
		auto replayedStairwell = std::dynamic_pointer_cast<const core::StairwellTransit>(
			replayed.getSector(movedIndex));
		require(replayedStairwell && replayedStairwell->getCellX() == 4
			&& replayedStairwell->getCellY() == 1
			&& replayed.getStairwellOptions(movedIndex, replayedOptions)
			&& replayedOptions.mountSide == CORE_SIDE_RIGHT
			&& replayedOptions.directionalCapacity == 2
			&& replayedOptions.directionalBatchLimit == 3,
			"Edited Stairwell did not round-trip through YAML");

		auto blocked = world.planResizeStairwell(movedIndex, 9, 1, edited);
		require(!blocked.valid, "Out-of-bounds Stairwell edit was accepted");
		auto removal = world.planRemoveStairwell(movedIndex);
		require(removal.valid && removal.requiresConfirmation(),
			"Stairwell deletion was not planned as a confirmed edit");
		require(world.applyStairwellEdit(removal) == ~0u,
			"Stairwell deletion did not return the removed-sector sentinel");
		require(!static_cast<core::World const&>(world).getLayer(1)
			->getCellDefinition(4, 1).occupied(),
			"Deleted Stairwell still occupies the Back layer");
	}

	void stairwellEditsReplayLocationsBeforeTransits(smoke::Context const&)
	{
		core::World world("Stairwell landing order", 16, 6);
		world.addCorridor(0u, 1, 0, 16, 1);
		world.addCorridor(0u, 2, 0, 7, 1);
		auto created = world.addStairwell(1, 1, 2,
			core::World::CreateStairwellOptions{ 2, CORE_SIDE_LEFT });
		world.addRoom("Room 1", 0, 3, 0, 7, 1);
		world.finishBuild();
		world.pauseSimulation();

		core::World::CreateStairwellOptions extended{ 3, CORE_SIDE_LEFT };
		auto plan = world.planResizeStairwell(created.sectorIndex, 2, 1, extended);
		require(plan.valid, "Extending a Stairwell onto a Location authored after it was rejected");
		auto edited = world.applyStairwellEdit(plan);
		auto stairwell = std::dynamic_pointer_cast<const core::StairwellTransit>(
			world.getSector(edited));
		require(stairwell && stairwell->getLevelsHigh() == 3,
			"The extended Stairwell does not span the new level");
	}

	void stairwellEditsReplayWalkwaysBeforeTransits(smoke::Context const&)
	{
		core::World world("Stairwell walkway order", 16, 5);
		auto room = world.addRoom("Room", 0, 0, 9, 5, 5);
		for (uint32_t x = 0; x < 5; ++x) world.addSectorWalkway(room, 1, x);
		for (uint32_t x = 3; x < 5; ++x) world.addSectorWalkway(room, 2, x);

		core::World::CreateLiftOptions liftOptions;
		liftOptions.stopOffsets = { 0, 1 };
		world.addSectorPlatformLift(room, 0, 0, liftOptions);
		auto created = world.addStairwell(1, 0, 12,
			core::World::CreateStairwellOptions{ 2, CORE_SIDE_RIGHT });
		world.finishBuild();
		world.pauseSimulation();

		core::World::CreateStairwellOptions extended{ 3, CORE_SIDE_RIGHT };
		auto plan = world.planResizeStairwell(created.sectorIndex, 12, 0, extended);
		require(plan.valid,
			"Extending a Stairwell onto authored Walkways replayed the Transit first");
		auto edited = world.applyStairwellEdit(plan);
		auto stairwell = std::dynamic_pointer_cast<const core::StairwellTransit>(
			world.getSector(edited));
		require(stairwell && stairwell->getLevelsHigh() == 3,
			"The Stairwell was not extended over its Walkway landing floors");
	}

	void ladderEditsReplayLocationsBeforeTransits(smoke::Context const&)
	{
		core::World world("Ladder landing order", 16, 6);
		world.addCorridor(0u, 1, 0, 16, 1);
		world.addCorridor(0u, 2, 0, 7, 1);
		auto created = world.addLadder(1, 1, 2, { 2, false, true });
		world.addCorridor(0u, 3, 0, 7, 1);
		world.finishBuild();
		world.pauseSimulation();

		core::World::CreateLadderOptions extended{ 3, false, true };
		auto plan = world.planResizeLadder(created.ladder.sector->getIndex(), 2, 1, extended);
		require(plan.valid, "Extending a Ladder onto a Location authored after it was rejected");
		auto edited = world.applyLadderEdit(plan);
		auto ladder = std::dynamic_pointer_cast<const core::LadderTransit>(
			world.getSector(edited));
		require(ladder && ladder->getLevelsHigh() == 3,
			"The extended Ladder does not span the new level");
	}

	void staircaseEditsReplayLocationsBeforeTransits(smoke::Context const&)
	{
		core::World world("Staircase landing order", 16, 6);
		world.addCorridor(0u, 1, 0, 16, 1);
		world.addCorridor(0u, 2, 0, 16, 1);
		auto index = world.addStaircase(1, 1, 2, { 2, CORE_SIDE_RIGHT, 0.0f });
		world.addCorridor(0u, 3, 0, 7, 1);
		world.finishBuild();
		world.pauseSimulation();

		core::World::CreateStaircaseOptions moved{ 2, CORE_SIDE_RIGHT, 0.0f };
		auto plan = world.planResizeStaircase(index, 2, 2, moved);
		require(plan.valid && plan.move, "Moving a Staircase onto a Location authored after it was rejected");
		auto edited = world.applyStaircaseEdit(plan);
		auto staircase = std::dynamic_pointer_cast<const core::StaircaseTransit>(
			world.getSector(edited));
		require(staircase && staircase->getCellY() == 2,
			"The moved Staircase did not reach the new level");
	}
}
