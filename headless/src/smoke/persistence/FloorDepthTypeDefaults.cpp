// The author's per-Sector-type floor depth scheme is saved, loaded, and
// survives the rebuild (#1103).
//
// The World tier pins what a type default means and the authoring tier pins
// what it means to undo one. Neither says anything about the document, and
// the document is where a scheme like this either lives or does not: the
// editor's live World is torn down and replayed from records on every
// structural edit, so a type map held only in memory would be gone the first
// time the author moved a wall.
//
// What is pinned here:
//
//   * the round trip, in both wire formats, for positive and negative
//     depths. A field that survives YAML and not binary is a field half the
//     saves in the game lose, and a signed field that comes back as its
//     absolute value is the classic way this goes wrong;
//   * all eleven types round-trip together. A scheme that loses a row on the
//     way out - or merges two - is a scheme the author can only discover by
//     noticing the picture is wrong after a reload;
//   * sparseness. A World with no type opinions writes no `sectorTypeFloorDepths`
//     field at all, and neither does a World whose only type opinion was
//     cleared. Zero lifts nothing, which is exactly what no field already
//     says, so writing a zero would invent a third state that means the
//     absence without being it;
//   * clearing survives the round trip. A cleared type must come back as no
//     entry, not as the depth it used to carry restored from somewhere -
//     which is the case a writer that only ever adds would get wrong;
//   * the version guard, in both directions. A document written before the
//     field existed - schema 72, no field - loads as no type opinions and
//     draws the flat picture it drew before, with no migration pass. A
//     document claiming to predate the feature while carrying the field did
//     not come from this writer and is refused rather than guessed at;
//   * malformed entries are refused rather than half-loaded. An unknown type
//     name, a zero entry, and a duplicated type are each a document this
//     writer could not have produced, and silently skipping them would leave
//     the author with a scheme quietly missing a row;
//   * the rebuild. A loaded World's type map drives the same effective
//     depths and the same Layer staircase the live one did, so the picture
//     after a reload is the picture before it.
//
// What may not change is everything else. A type default is render state, and
// the document says so: same Sectors, same ends, same capacity, same
// construction in every other field.

#include "WorldChecks.h"

#include "core/BinarySerializer.h"
#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace persistence
{
	using smoke::require;
	using WorldPtr = std::shared_ptr<core::World>;

	namespace
	{
		std::string serialize(core::World const& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		}

		std::string serializeBinary(core::World const& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::BinarySerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		}

		WorldPtr load(std::string const& yaml)
		{
			auto loaded = std::make_shared<core::World>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			require(loaded->deserialize(*reader, workData),
				"The authored World document did not load");
			return loaded;
		}

		WorldPtr loadBinary(std::string const& bytes)
		{
			auto loaded = std::make_shared<core::World>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::BinarySerializer::fromString(bytes);
			reader->deserialize();
			require(loaded->deserialize(*reader, workData),
				"The authored World binary document did not load");
			return loaded;
		}

		// A two-Layer World with a Room and a Background on the front Layer
		// and a Room behind, so a type default has something to move and
		// something to lift.
		WorldPtr twoLayerWorld(std::string const& name)
		{
			auto world = std::make_shared<core::World>(name, 20, 8);
			while (world->getLayerCount() < 2) world->addLayer();
			world->addRoom("Front", 0, 2, 1, 3, 2);
			world->addBackground(0, 2, 8, 3, 2);
			world->addRoom("Behind", 1, 2, 1, 3, 2);
			return world;
		}

		uint32_t sectorOfType(core::World const& world, core::SectorType type,
			uint32_t layer = 0)
		{
			std::vector<uint32_t> found;
			for (auto const& sector : world.getSectors(layer))
				if (sector->getType() == type) found.push_back(sector->getIndex());
			require(found.size() == 1, "Layer " + std::to_string(layer) + " holds "
				+ std::to_string(found.size()) + " Sectors of type "
				+ core::getSectorTypeString(type) + "; the check needs exactly one");
			return found.front();
		}

		bool refused(std::function<void()> action)
		{
			try { action(); }
			catch (std::exception const&) { return true; }
			return false;
		}

		// The fixture World's document with its type map replaced by exactly
		// these entries. Building the document by hand rather than editing a
		// saved one keeps the malformed cases honest about being malformed
		// rather than about the writer's own shape.
		std::string withEntries(core::World const& world,
			std::vector<std::pair<std::string, int32_t>> const& entries)
		{
			YAML::Node doc = YAML::Load(serialize(world));
			doc["sectorTypeFloorDepths"] = YAML::Node(YAML::NodeType::Sequence);
			for (auto const& [type, depth] : entries)
			{
				YAML::Node entry{ YAML::NodeType::Map };
				entry["type"] = type;
				entry["depth"] = depth;
				doc["sectorTypeFloorDepths"].push_back(entry);
			}
			return YAML::Dump(doc);
		}

		int32_t countField(core::World const& world, std::string const& field)
		{
			int32_t found = 0;
			auto const yaml = serialize(world);
			for (size_t at = yaml.find(field); at != std::string::npos;
					at = yaml.find(field, at + field.size()))
				++found;
			return found;
		}

		// Every type's default in a loaded World must match the one that was
		// authored, and no other type may have picked one up.
		void requireSameMap(core::World const& loaded, core::World const& authored)
		{
			for (auto const type : core::allSectorTypes())
			{
				auto const name = core::getSectorTypeString(type);
				require(loaded.sectorTypeFloorDepth(type)
						== authored.sectorTypeFloorDepth(type),
					"Type " + name + " came back as "
					+ std::to_string(loaded.sectorTypeFloorDepth(type))
					+ " rather than the "
					+ std::to_string(authored.sectorTypeFloorDepth(type)) + " authored");
				require(loaded.hasSectorTypeFloorDepth(type)
						== authored.hasSectorTypeFloorDepth(type),
					"Type " + name + " reports entry presence "
					+ std::to_string(loaded.hasSectorTypeFloorDepth(type))
					+ " against the authored "
					+ std::to_string(authored.hasSectorTypeFloorDepth(type)));
			}
		}
	}

	// A type default comes back out of both wire formats exactly as it went
	// in, and the Layer behind still draws by it after the reload.
	void typeDefaultRoundTrips(smoke::Context const&)
	{
		auto const world = twoLayerWorld("Type default round trip");
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"the type default write was refused in the fixture");

		auto const fromYaml = load(serialize(*world));
		require(fromYaml->sectorTypeFloorDepth(core::SectorType::Location) == 24,
			"A type default of 24 came back from YAML as "
			+ std::to_string(fromYaml->sectorTypeFloorDepth(core::SectorType::Location)));
		require(fromYaml->effectiveFloorDepth(sectorOfType(*fromYaml,
			core::SectorType::Location)) == 24,
			"The reloaded Room does not follow its type");
		require(fromYaml->layerFloorOffsetPx(1) == 24,
			"The Layer behind does not draw 24px off after a YAML reload");

		auto const fromBinary = loadBinary(serializeBinary(*world));
		require(fromBinary->sectorTypeFloorDepth(core::SectorType::Location) == 24,
			"A type default of 24 came back from binary as "
			+ std::to_string(fromBinary->sectorTypeFloorDepth(core::SectorType::Location)));
		require(fromBinary->layerFloorOffsetPx(1) == 24,
			"The Layer behind does not draw 24px off after a binary reload");
	}

	// A negative type default comes back negative in both formats. A signed
	// field that comes back as its absolute value is the classic way this
	// goes wrong, and a Background pushed down is exactly what an author
	// would want.
	void aNegativeTypeDefaultRoundTrips(smoke::Context const&)
	{
		auto const world = twoLayerWorld("Negative type default");
		require(world->setSectorTypeFloorDepth(core::SectorType::Background, -20).changed,
			"the negative type default write was refused in the fixture");

		auto const fromYaml = load(serialize(*world));
		require(fromYaml->sectorTypeFloorDepth(core::SectorType::Background) == -20,
			"A type default of -20 came back from YAML as "
			+ std::to_string(fromYaml->sectorTypeFloorDepth(core::SectorType::Background)));
		require(fromYaml->effectiveFloorDepth(sectorOfType(*fromYaml,
			core::SectorType::Background)) == -20,
			"The reloaded Background does not follow its negative type default");

		auto const fromBinary = loadBinary(serializeBinary(*world));
		require(fromBinary->sectorTypeFloorDepth(core::SectorType::Background) == -20,
			"A type default of -20 came back from binary as "
			+ std::to_string(fromBinary->sectorTypeFloorDepth(core::SectorType::Background)));
	}

	// All eleven types carry distinct depths through both formats at once.
	// A scheme that loses a row on the way out, or merges two rows into one,
	// is a scheme the author can only discover by noticing the picture is
	// wrong after a reload.
	void everyTypeRoundTripsTogether(smoke::Context const&)
	{
		auto const world = twoLayerWorld("Eleven type defaults");
		auto const& types = core::allSectorTypes();
		for (size_t i = 0; i < types.size(); ++i)
		{
			auto const depth = static_cast<int32_t>(i + 1) * 7 - 38;
			require(world->setSectorTypeFloorDepth(types[i], depth).changed,
				"Setting a default on type " + core::getSectorTypeString(types[i])
				+ " was refused in the fixture");
		}

		requireSameMap(*load(serialize(*world)), *world);
		requireSameMap(*loadBinary(serializeBinary(*world)), *world);
	}

	// Sparseness: a World with no type opinions writes no field at all, and
	// neither does a World whose only type opinion was cleared.
	void anEmptyTypeMapWritesNoField(smoke::Context const&)
	{
		auto const world = twoLayerWorld("No type opinions");
		require(countField(*world, "sectorTypeFloorDepths") == 0,
			"A World with no type opinions wrote a sectorTypeFloorDepths field");

		// And it loads back as no type opinions rather than as a malformed
		// or partially-populated document.
		auto const reloaded = load(serialize(*world));
		requireSameMap(*reloaded, *world);
		require(reloaded->layerFloorOffsetPx(1) == 0,
			"The reloaded World draws off with no type opinions recorded");
	}

	// Clearing survives the round trip: a cleared type comes back as no
	// entry, not as the depth it used to carry. This is the case a writer
	// that only ever adds entries would get wrong, and the reason the
	// loader assigns the whole map rather than merging into whatever the
	// World was built with.
	void aClearedTypeRoundTripsAsNoEntry(smoke::Context const&)
	{
		auto const world = twoLayerWorld("Cleared type round trip");
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"type default refused in fixture");
		require(countField(*world, "sectorTypeFloorDepths") == 1,
			"A World with one type opinion wrote "
			+ std::to_string(countField(*world, "sectorTypeFloorDepths"))
			+ " sectorTypeFloorDepths fields");
		require(world->clearSectorTypeFloorDepth(core::SectorType::Location).changed,
			"clearing the type default was not seen as a change");

		require(countField(*world, "sectorTypeFloorDepths") == 0,
			"A World whose only type opinion was cleared still wrote a "
			"sectorTypeFloorDepths field");
		requireSameMap(*load(serialize(*world)), *world);
		requireSameMap(*loadBinary(serializeBinary(*world)), *world);
		require(load(serialize(*world))->layerFloorOffsetPx(1) == 0,
			"The reloaded World still draws off after the type that lifted it "
			"was cleared");
	}

	// A World carrying two type opinions keeps both distinct through the
	// round trip, including when one is cleared and the other is not.
	void aPartiallyClearedMapRoundTrips(smoke::Context const&)
	{
		auto const world = twoLayerWorld("Partially cleared");
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"Room default refused in fixture");
		require(world->setSectorTypeFloorDepth(core::SectorType::Background, -8).changed,
			"Background default refused in fixture");
		require(world->clearSectorTypeFloorDepth(core::SectorType::Location).changed,
			"clear refused");

		auto const fromYaml = load(serialize(*world));
		requireSameMap(*fromYaml, *world);
		require(!fromYaml->hasSectorTypeFloorDepth(core::SectorType::Location),
			"The cleared Room type came back with an entry");
		require(fromYaml->sectorTypeFloorDepth(core::SectorType::Background) == -8,
			"The Background default was lost when the Room default was cleared");
	}

	// The version guard, both directions. A document written before the field
	// existed loads as no type opinions and draws the flat picture it drew
	// before, with no migration pass. A document claiming to predate the
	// feature while carrying the field did not come from this writer.
	void theVersionGuardHoldsBothWays(smoke::Context const&)
	{
		auto const world = twoLayerWorld("Version guard");
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"type default refused in fixture");
		auto const yaml = serialize(*world);
		require(yaml.find("version: 73") != std::string::npos,
			"The writer did not emit schema 73, the version the type map needs");
		require(yaml.find("sectorTypeFloorDepths") != std::string::npos,
			"The type map was not written at all");

		// Schema 72 predates the field: a document at 72 with no field is
		// the ordinary pre-feature case and loads clean.
		YAML::Node preFeature = YAML::Load(yaml);
		preFeature["version"] = 72;
		preFeature.remove("sectorTypeFloorDepths");
		auto const stripped = YAML::Dump(preFeature);
		require(stripped.find("sectorTypeFloorDepths") == std::string::npos,
			"Stripping the field from the pre-feature document left it present");
		auto const loaded = load(stripped);
		require(!loaded->hasSectorTypeFloorDepth(core::SectorType::Location),
			"A pre-feature document loaded a type opinion");
		require(loaded->layerFloorOffsetPx(1) == 0,
			"A pre-feature document does not draw the flat picture it drew before");

		// Schema 72 claiming to carry the field did not come from this
		// writer and is refused rather than guessed at.
		YAML::Node lying = YAML::Load(yaml);
		lying["version"] = 72;
		require(YAML::Dump(lying).find("sectorTypeFloorDepths") != std::string::npos,
			"fixture: the lying document lost the field it is lying about");
		require(refused([&] { load(YAML::Dump(lying)); }),
			"A schema 72 document carrying the type map loaded instead of being "
			"refused");
	}

	// Malformed entries are refused rather than half-loaded. Each of these is
	// a document this writer could not have produced, and silently skipping
	// one would leave the author with a scheme quietly missing a row.
	void aMalformedTypeMapIsRefused(smoke::Context const&)
	{
		auto const world = twoLayerWorld("Malformed type map");
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"type default refused in fixture");

		// An unknown type name.
		require(refused([&] {
				load(withEntries(*world, { { "NotAType", 12 } })); }),
			"A document naming a type that does not exist loaded");

		// A zero entry, which this writer never produces.
		require(refused([&] {
				load(withEntries(*world, { { "Room", 0 } })); }),
			"A document carrying a zero type entry loaded; zero is stored as "
			"no entry, so a zero entry is not a state this writer produces");

		// A duplicated type.
		require(refused([&] {
				load(withEntries(*world, { { "Room", 10 }, { "Room", 11 } })); }),
			"A document carrying the same type twice loaded");

		// A well-formed map beside a malformed one is refused as a whole, not
		// half-loaded: the good row must not survive the bad one.
		require(refused([&] {
				load(withEntries(*world, { { "Room", 14 }, { "NotAType", 3 } })); }),
			"A document with one good row and one bad row loaded the good row");
	}

	// A type default is render state, and the document says so: the round
	// trip changes the type map and nothing else. Same Sectors, same ends,
	// same capacity, same construction in every other field.
	void aTypeDefaultChangesNothingElseInTheDocument(smoke::Context const&)
	{
		auto const plain = twoLayerWorld("Nothing else changes");
		auto const lifted = twoLayerWorld("Nothing else changes");
		require(lifted->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"type default refused in fixture");

		auto const plainYaml = serialize(*plain);
		auto const liftedYaml = serialize(*lifted);
		require(plainYaml != liftedYaml,
			"A type default made no difference to the document at all");

		// The only difference is the version line and the type map. Strip
		// both from the lifted document and it must read as the plain one.
		auto stripped = YAML::Load(liftedYaml);
		stripped.remove("sectorTypeFloorDepths");
		auto const strippedYaml = YAML::Dump(stripped);
		auto const plainDoc = YAML::Load(plainYaml);
		require(YAML::Dump(plainDoc).find("sectorTypeFloorDepths") == std::string::npos,
			"The plain document carried a type map");
		require(strippedYaml.find("sectorTypeFloorDepths") == std::string::npos,
			"Removing the field left it present");

		// Same Sector count, same Layer count, same names on both sides.
		auto const a = load(plainYaml);
		auto const b = load(liftedYaml);
		require(a->getNumSectors() == b->getNumSectors(),
			"A type default changed the Sector count");
		require(a->getLayerCount() == b->getLayerCount(),
			"A type default changed the Layer count");
		require(a->getName() == b->getName(),
			"A type default changed the World name");
	}
}
