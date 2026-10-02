#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

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

namespace persistence
{
	using smoke::require;

	void worldRoundTripsAuthoredStateAndAgents(smoke::Context const&)
	{
		core::World original("Serializable world", 8, 3);
		auto const fore = original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.activationMode = core::DoorActivationMode::RemoteControlled;
		doorOptions.controls[0] = true;
		doorOptions.controls[1] = true;
		doorOptions.crossingLanes = 2;
		original.addSectorDoor(0, 0, 3, doorOptions);
		auto const removedMarker = original.addSectorMarker(fore, 0, 1.5f);
		uint32_t destinationIdentifier{ 0x53455231u };
		original.addSectorMarker(fore, 0, 2.5f, &destinationIdentifier);
		require(original.removeSectorMarker(fore, removedMarker.index),
			"Marker could not be removed through World");
		require(!original.removeSectorMarker(fore, removedMarker.index),
			"Marker deletion accepted an empty object slot");
		original.finishBuild();
		auto const agentId = original.createAgent("Serialized agent", fore, 0, 0.75f);
		auto* originalAgent = original.lookupAgent(agentId).entity;
		originalAgent->setFlags(0x12u);
		auto destination = original.getGraph()->getVertexByIdentifier(destinationIdentifier);
		auto path = original.getGraph()->calculatePath(originalAgent, destination);
		require(path && !path->nodes.empty(), "Agent path could not be created for serialization");
		originalAgent->setPath(std::move(path), true);

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("version: 39") != std::string::npos
			&& yaml.find("layers: 2") != std::string::npos
			&& yaml.find("layerNames:") != std::string::npos
			&& yaml.find("- Layer 0") != std::string::npos
			&& yaml.find("- Layer 1") != std::string::npos
			&& yaml.find("type: room") != std::string::npos
			&& yaml.find("cellsWide:") != std::string::npos
			&& yaml.find("levelsHigh:") != std::string::npos
			&& yaml.find("levelIndex:") != std::string::npos
			&& yaml.find("foreControl: true") != std::string::npos
			&& yaml.find("\n    a:") == std::string::npos,
			"World YAML did not use the explicit Level construction schema");
		require(yaml.find("construction") != std::string::npos
			&& yaml.find("agents") != std::string::npos
			&& yaml.find("path:") != std::string::npos
			&& yaml.find("destinationSector:") != std::string::npos
			&& yaml.find("active: true") != std::string::npos,
			"World YAML omitted authored structure, agents, or Agent paths");

		core::World loaded("placeholder", 2, 2);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "World deserialization failed");
		require(loaded.getName() == original.getName()
			&& loaded.getCellsWide() == original.getCellsWide()
			&& loaded.getLevelsHigh() == original.getLevelsHigh()
			&& loaded.getLayerCount() == original.getLayerCount()
			&& loaded.getLayerName(0) == "Layer 0"
			&& loaded.getLayerName(1) == "Layer 1"
			&& loaded.getNumSectors() == original.getNumSectors(),
			"World metadata or sectors did not round-trip");
		require(loaded.getGraph() && !loaded.getGraph()->getVertices().empty(),
			"World graph was not regenerated after deserialization");
		uint32_t markerCount{ 0 };
		auto const loadedFore = loaded.getSector(fore);
		for (uint32_t i = 0; i < loadedFore->getNumObjects(); ++i)
		{
			auto const object = loadedFore->getObject(i);
			if (object && object->getObjectType() == core::SectorObjectType::Marker) ++markerCount;
		}
		require(markerCount == 1, "Marker deletion did not round-trip");
		auto const loadedAgent = loaded.lookupAgent(agentId);
		require(loadedAgent && loadedAgent.entity->getName() == "Serialized agent"
			&& loadedAgent.entity->getFlags() == 0x12u
			&& loadedAgent.entity->getSector()->getIndex() == fore
			&& std::abs(loadedAgent.entity->getLocalPosition().x - 0.75f) < 0.0001f
			&& loadedAgent.entity->getState() == core::Agent::State::MovingToVertex
			&& loadedAgent.entity->getPath()
			&& loadedAgent.entity->getPath()->nodes.back().targetVertex->getSector()->getIndex() == fore
			&& std::abs(loadedAgent.entity->getPath()->nodes.back().targetVertex->getSectorOffset().x
				- destination->getSectorOffset().x) < 0.0001f,
			"World-owned Agent or its active path did not round-trip");
		require(!loaded.isModified(), "deserialized World was unexpectedly modified");

		// Version 15 changed the persisted vertical-position vocabulary. Documents
		// written by older builds retain their legacy field spellings and must
		// continue to open.
		auto legacyYaml = yaml;
		auto replaceAll = [&](std::string const& from, std::string const& to)
		{
			for (auto at = legacyYaml.find(from); at != std::string::npos;
				at = legacyYaml.find(from, at + to.size()))
			{
				legacyYaml.replace(at, from.size(), to);
			}
		};
		replaceAll("version: 39", "version: 14");
		replaceAll("levelsHigh", "decksHigh");
		replaceAll("levelIndex", "deckIndex");
		replaceAll("topLevelHeight", "topDeckHeight");
		core::World legacyLoaded("legacy placeholder", 2, 2);
		auto legacyReader = core::YamlSerializer::fromString(legacyYaml);
		legacyReader->deserialize();
		require(legacyLoaded.deserialize(*legacyReader, workData)
			&& legacyLoaded.getLevelsHigh() == original.getLevelsHigh()
			&& legacyLoaded.getNumSectors() == original.getNumSectors(),
			"A pre-Level-vocabulary World no longer loads");

		auto replacementPath = loaded.getGraph()->calculatePath(loadedAgent.entity,
			loadedAgent.entity->getPath()->nodes.back().targetVertex);
		require(replacementPath && !replacementPath->nodes.empty(),
			"Replacement Agent path could not be created");
		loadedAgent.entity->setPath(std::move(replacementPath), false);
		require(loaded.isModified(), "Setting an Agent path did not mark its World modified");
		loadedAgent.entity->clearPath();
		require(loaded.removeAgent(agentId).removed, "deserialized Agent could not be removed");
		require(loaded.isModified(), "removing an Agent did not modify its World");
	}

	void legacyWorldYamlStillLoads(smoke::Context const&)
	{
		auto const yaml = R"yaml(version: 1
name: Legacy
cellsWide: 4
levelsHigh: 2
construction:
  - kind: 0
    name: ""
    a: 0
    b: 0
    c: 4
    d: 1
    e: 0
    f: 0
    g: 0
    i: 0
    j: 0
    x: 0
    y: 0
    p: 0
    q: 0
    values: []
agents: []
)yaml";
		core::World loaded("placeholder", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "version 1 World YAML no longer loads");
		require(loaded.getName() == "Legacy" && loaded.getNumSectors() == 1,
			"version 1 World YAML loaded incorrectly");
	}

	void legacyVersion3WorldYamlStillLoadsWithDefaultLayers(smoke::Context const&)
	{
		auto const yaml = R"yaml(version: 3
name: Legacy v3
cellsWide: 4
levelsHigh: 2
construction:
  - type: corridor
    y: 0
    x: 0
    cellsWide: 4
    levelsHigh: 1
agents: []
)yaml";
		core::World loaded("placeholder", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "version 3 World YAML no longer loads");
		require(loaded.getName() == "Legacy v3"
			&& loaded.getLayerCount() == 2
			&& loaded.getLayerName(0) == "Layer 0"
			&& loaded.getLayerName(1) == "Layer 1",
			"version 3 World YAML loaded with wrong layer defaults");
	}

	void version4WorldYamlStillLoads(smoke::Context const&)
	{
		auto const yaml = R"yaml(version: 4
name: Legacy v4
cellsWide: 6
levelsHigh: 2
layers: 3
layerNames:
  - Ground
  - Mezzanine
  - Sublevel
construction:
  - type: room
    name: Ground room
    layer: 0
    y: 0
    x: 0
    cellsWide: 3
    levelsHigh: 1
    topLevelHeight: 0.9
  - type: room
    name: Deep room
    layer: 2
    y: 0
    x: 3
    cellsWide: 3
    levelsHigh: 1
    topLevelHeight: 0.9
agents: []
)yaml";
		core::World loaded("placeholder", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "version 4 World YAML no longer loads");
		core::World const& loadedRef = loaded;
		require(loaded.getName() == "Legacy v4"
			&& loaded.getLayerCount() == 3
			&& loadedRef.getNumSectors() == 2
			&& loadedRef.getSector(0)->getLayerIndex() == 0
			&& loadedRef.getSector(1)->getLayerIndex() == 2,
			"version 4 World YAML did not load into the same shape");
	}

	void layerFieldsAcceptLegacyNamesAndIndices(smoke::Context const&)
	{
		auto const yaml = R"yaml(version: 5
name: Mixed layer spellings
cellsWide: 6
levelsHigh: 2
layers: 3
layerNames:
  - Ground
  - Mezzanine
  - Sublevel
construction:
  - type: room
    name: Front
    layer: fore
    y: 0
    x: 0
    cellsWide: 3
    levelsHigh: 1
    topLevelHeight: 0.9
  - type: room
    name: Deep
    layer: 2
    y: 0
    x: 3
    cellsWide: 3
    levelsHigh: 1
    topLevelHeight: 0.9
agents: []
)yaml";
		core::World loaded("placeholder", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData),
			"A World mixing legacy layer names and layer indices did not load");
		core::World const& loadedRef = loaded;
		require(loadedRef.getNumSectors() == 2
			&& loadedRef.getSector(0)->getLayerIndex() == 0
			&& loadedRef.getSector(1)->getLayerIndex() == 2,
			"Legacy and indexed layers were not resolved to the right layers");
	}
}
