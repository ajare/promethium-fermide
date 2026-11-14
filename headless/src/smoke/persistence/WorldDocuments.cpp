#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
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
#include "core/BinarySerializer.h"
#include <yaml-cpp/yaml.h>

#include "WorldChecks.h"

namespace persistence
{
	using smoke::require;

	namespace
	{
		// The four atlas fields rewritten on one otherwise-identical document, so
		// the only thing a load can react to is the selection under test. A null
		// name removes the field, which is what a pre-selection document looks
		// like.
		std::string withAtlases(std::string const& base, char const* sector, char const* object,
			char const* agent, char const* furniture)
		{
			auto document = YAML::Load(base);
			auto const apply = [&](char const* field, char const* value) {
				if (value) document[field] = value;
				else document.remove(field);
			};
			apply("sectorImageSet", sector);
			apply("objectImageSet", object);
			apply("agentImageSet", agent);
			apply("furnitureImageSet", furniture);
			return YAML::Dump(document);
		}

		std::unique_ptr<core::World> loadAtlasDocument(std::string const& yaml, std::string const& label)
		{
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			auto world = std::make_unique<core::World>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			if (!world->deserialize(*reader, workData))
				throw std::runtime_error("Atlas document failed to load: " + label);
			return world;
		}

		// One World's four selections, read back as a tuple of names so a check
		// can compare all four in one assertion.
		std::vector<std::string> atlasNames(core::World const& world)
		{
			return { world.sectorImageSetResourceName(), world.objectImageSetResourceName(),
				world.agentImageSetResourceName(), world.furnitureImageSetResourceName() };
		}

		std::vector<std::string> expectedNames(char const* sector, char const* object,
			char const* agent, char const* furniture)
		{
			return { sector, object, agent, furniture };
		}
	}

	// The four canonical sets are what a World created from nothing asks for,
	// and what a document that names none reads. Since the #1013 terminus
	// dropped the `New` suffix there is one family of transition sets and its
	// name is the plain one, so a fresh World and a pre-selection document
	// agree on all four names.
	void worldsDefaultToTheCanonicalAtlases(smoke::Context const&)
	{
		core::World fresh("Canonical atlas defaults", 4, 2);
		require(fresh.sectorImageSetResourceName() == "SectorAtlas",
			"A new World does not default to SectorAtlas");
		require(fresh.objectImageSetResourceName() == "ObjectAtlas",
			"A new World does not default to ObjectAtlas");
		require(fresh.agentImageSetResourceName() == "AgentAtlas",
			"A new World does not default to AgentAtlas");
		require(fresh.furnitureImageSetResourceName() == "FurnitureAtlas",
			"A new World does not default to FurnitureAtlas");

		// The defaults round-trip through the document they are written into.
		fresh.addRoom("Round-trip room", 0, 0, 0, 3, 2);
		fresh.finishBuild();
		core::SerializationWorkData writeData;
		auto const writer = core::YamlSerializer::toString();
		fresh.serialize(*writer, writeData);
		writer->serialize();
		auto const base = writer->getSerializedString();
		for (auto const* field : { "sectorImageSet: SectorAtlas", "objectImageSet: ObjectAtlas",
				"agentImageSet: AgentAtlas", "furnitureImageSet: FurnitureAtlas" })
			require(base.find(field) != std::string::npos,
				std::string("A new World did not write its default: ") + field);

		auto const roundTripped = loadAtlasDocument(base, "canonical defaults");
		require(atlasNames(*roundTripped) == expectedNames("SectorAtlas", "ObjectAtlas",
			"AgentAtlas", "FurnitureAtlas"), "The canonical atlas defaults did not round-trip");
		require(roundTripped->getLoadWarnings().empty(),
			"The canonical atlas defaults warned on load: "
			+ (roundTripped->getLoadWarnings().empty() ? std::string{} : roundTripped->getLoadWarnings().front()));
	}

	// Nothing re-skins under the author: a document that names its four sets -
	// custom themes included - reads back exactly what it named, and a document
	// that names none reads the canonical four rather than whatever the World it
	// was saved from was wearing.
	void existingWorldsKeepTheirStoredAtlasSelections(smoke::Context const&)
	{
		core::World base("Atlas selection base", 4, 2);
		base.addRoom("Selection room", 0, 0, 0, 3, 2);
		base.finishBuild();
		core::SerializationWorkData writeData;
		auto const writer = core::YamlSerializer::toString();
		base.serialize(*writer, writeData);
		writer->serialize();
		auto const document = writer->getSerializedString();

		auto const themed = loadAtlasDocument(withAtlases(document,
			"StationAtlas", "CustomObjects", "CustomAgents", "StationFurnitureAtlas"),
			"custom theme");
		require(atlasNames(*themed) == expectedNames("StationAtlas", "CustomObjects",
			"CustomAgents", "StationFurnitureAtlas"),
			"A themed document did not read back the sets it named");
		require(themed->getLoadWarnings().empty(),
			"A themed World warned on load: "
			+ (themed->getLoadWarnings().empty() ? std::string{} : themed->getLoadWarnings().front()));

		// A document predating the four selections names none of them and reads
		// the canonical four - the same four names a freshly created World
		// carries, which is what makes the terminus a terminus rather than a
		// second default to remember.
		auto const unselected = loadAtlasDocument(
			withAtlases(document, nullptr, nullptr, nullptr, nullptr), "unselected");
		require(atlasNames(*unselected) == expectedNames("SectorAtlas", "ObjectAtlas",
			"AgentAtlas", "FurnitureAtlas"),
			"A document naming no atlases did not read the canonical defaults");
		require(unselected->getLoadWarnings().empty(), "A document naming no atlases warned on load");
	}

	void worldRoundTripsAuthoredStateAndAgents(smoke::Context const&)
	{
		core::World original("Serializable world", 8, 3);
		require(original.sectorImageSetResourceName() == "SectorAtlas", "New World artwork must default to SectorAtlas");
		original.setSectorImageSetResourceName("StationAtlas");
		require(original.agentImageSetResourceName() == "AgentAtlas", "New Worlds must default to AgentAtlas");
		original.setAgentImageSetResourceName("CustomAgents");
		require(original.furnitureImageSetResourceName() == "FurnitureAtlas", "New Worlds must default to FurnitureAtlas");
		original.setFurnitureImageSetResourceName("CustomFurniture");
		// The fourth atlas selection (#1022), defaulting to its replacement set
		// with the other three (#1023).
		require(original.objectImageSetResourceName() == "ObjectAtlas",
			"New Worlds must default to ObjectAtlas");
		original.setObjectImageSetResourceName("CustomObjects");
		require(original.isModified(), "Selecting sector artwork must modify the World");
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
		require(yaml.find("version: 73") != std::string::npos
			&& yaml.find("layerCount: 2") != std::string::npos
			&& yaml.find("layers:") != std::string::npos
			&& yaml.find("name: Layer 0") != std::string::npos
			&& yaml.find("name: Layer 1") != std::string::npos
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
			&& yaml.find("active: true") != std::string::npos
			&& yaml.find("action: idle") != std::string::npos,
			"World YAML omitted authored structure, agents, or Agent paths");

		core::World loaded("placeholder", 2, 2);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "World deserialization failed");
		require(loaded.sectorImageSetResourceName() == "StationAtlas", "Sector ImageSet did not round-trip");
		require(loaded.agentImageSetResourceName() == "CustomAgents", "Agent ImageSet did not round-trip");
		require(loaded.furnitureImageSetResourceName() == "CustomFurniture", "Furniture ImageSet did not round-trip");
		require(loaded.objectImageSetResourceName() == "CustomObjects", "Objects ImageSet did not round-trip");
		require(yaml.find("objectImageSet: CustomObjects") != std::string::npos,
			"The Objects ImageSet selection was not written into the document");
		auto binaryWriter = core::BinarySerializer::toString();
		original.serialize(*binaryWriter, workData); binaryWriter->serialize();
		auto binaryReader = core::BinarySerializer::fromString(binaryWriter->getSerializedString());
		binaryReader->deserialize();
		core::World binaryLoaded("Binary artwork", 2, 2);
		require(binaryLoaded.deserialize(*binaryReader, workData)
			&& binaryLoaded.furnitureImageSetResourceName() == "CustomFurniture"
			&& binaryLoaded.agentImageSetResourceName() == "CustomAgents"
			&& binaryLoaded.sectorImageSetResourceName() == "StationAtlas"
			&& binaryLoaded.objectImageSetResourceName() == "CustomObjects",
			"Binary artwork selection did not round-trip");
		for (bool missing : {false, true}) {
			auto document = YAML::Load(yaml);
			if (missing) document.remove("objectImageSet");
			else document["objectImageSet"] = "UnavailableObjects";
			auto artworkReader = core::YamlSerializer::fromString(YAML::Dump(document));
			artworkReader->deserialize();
			core::World artworkLoaded("Objects artwork", 2, 2);
			require(artworkLoaded.deserialize(*artworkReader, workData), "Objects artwork document failed to load");
			require(artworkLoaded.objectImageSetResourceName() == (missing ? "ObjectAtlas" : "UnavailableObjects"),
				"Absent Objects artwork did not default or unavailable name was lost");
		}
		for (bool missing : {false, true}) {
			auto document = YAML::Load(yaml);
			if (missing) document.remove("furnitureImageSet");
			else document["furnitureImageSet"] = "UnavailableFurniture";
			auto artworkReader = core::YamlSerializer::fromString(YAML::Dump(document));
			artworkReader->deserialize();
			core::World artworkLoaded("Furniture artwork", 2, 2);
			require(artworkLoaded.deserialize(*artworkReader, workData), "Furniture artwork document failed to load");
			require(artworkLoaded.furnitureImageSetResourceName() == (missing ? "FurnitureAtlas" : "UnavailableFurniture"),
				"Absent Furniture artwork did not default or unavailable name was lost");
		}
		for (bool missing : {false, true}) {
			auto document = YAML::Load(yaml);
			if (missing) document.remove("agentImageSet");
			else document["agentImageSet"] = "UnavailableAgents";
			auto artworkReader = core::YamlSerializer::fromString(YAML::Dump(document));
			artworkReader->deserialize();
			core::World artworkLoaded("Agent artwork", 2, 2);
			require(artworkLoaded.deserialize(*artworkReader, workData), "Agent artwork document failed to load");
			require(artworkLoaded.agentImageSetResourceName() == (missing ? "AgentAtlas" : "UnavailableAgents"),
				"Absent Agent artwork did not default or invalid name was lost");
		}
		for (bool missing : {false, true})
		{
			auto artworkYaml = yaml;
			auto field = artworkYaml.find("sectorImageSet:");
			require(field != std::string::npos, "World omitted sector artwork resource name");
			auto begin = artworkYaml.rfind('\n', field) + 1;
			auto end = artworkYaml.find('\n', field);
			if (missing) artworkYaml.erase(begin, end - begin + 1);
			else artworkYaml.replace(field, end - field, "sectorImageSet: UnavailableArtwork");
			auto artworkReader = core::YamlSerializer::fromString(artworkYaml);
			artworkReader->deserialize();
			core::World artworkLoaded("Artwork", 2, 2);
			require(artworkLoaded.deserialize(*artworkReader, workData), "Artwork document failed to load");
			require(artworkLoaded.sectorImageSetResourceName() == (missing ? "SectorAtlas" : "UnavailableArtwork"),
				"Absent artwork did not default or unavailable resource name was lost");
		}
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
		require(loaded.getSimulationSnapshot().agents.front().selectedAction == core::IdleAction,
			"Document reconstruction lost Idle intent");
		require(!loaded.isModified(), "deserialized World was unexpectedly modified");
		for (bool invalid : { false, true })
		{
			auto actionYaml = yaml;
			auto field = actionYaml.find("action: idle");
			require(field != std::string::npos, "Missing serialized Action intent");
			if (invalid) actionYaml.replace(field, std::string("action: idle").size(), "action: sit");
			else
			{
				auto begin = actionYaml.rfind('\n', field) + 1;
				actionYaml.erase(begin, actionYaml.find('\n', field) - begin + 1);
			}
			auto actionReader = core::YamlSerializer::fromString(actionYaml);
			actionReader->deserialize();
			core::World actionLoaded("Action document", 2, 2);
			bool refused = false;
			try { require(actionLoaded.deserialize(*actionReader, workData), "Action document load failed"); }
			catch (core::SerializationException const&) { refused = true; }
			require(refused == invalid, "Absent Action did not default to Idle or invalid Action fell back");
			if (!invalid) require(actionLoaded.getSimulationSnapshot().agents.front().selectedAction == core::IdleAction,
				"Missing Action field lost default Idle");
		}

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
		replaceAll("version: 73", "version: 14");
		if (auto field = legacyYaml.find("destinationMarker:"); field != std::string::npos)
		{
			auto begin = legacyYaml.rfind('\n', field) + 1;
			legacyYaml.erase(begin, legacyYaml.find('\n', field) - begin + 1);
		}
		if (auto field = legacyYaml.find("action: idle"); field != std::string::npos)
		{
			auto begin = legacyYaml.rfind('\n', field) + 1;
			legacyYaml.erase(begin, legacyYaml.find('\n', field) - begin + 1);
		}
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
