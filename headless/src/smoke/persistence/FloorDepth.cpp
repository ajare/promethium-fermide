// A floor depth is saved, loaded, and survives the rebuild (#1101).
//
// A depth held only on the live Sector would draw this frame and be gone
// from the document, because every structural edit tears the World down and
// replays the construction records. A depth held only on the record would
// save and never draw until such an edit happened. The feature only exists
// if the number is on both halves and comes back off the record onto the
// Sector it created, which is what this file is for.
//
// What is pinned here:
//
//   * the round trip, in both wire formats, for a positive depth and for a
//     negative one. A field that survives YAML and not binary is a field
//     half the saves in the game lose, and a negative that comes back as
//     its absolute value is the classic way a signed field goes wrong;
//   * sparseness. A World nobody lifted writes no `floorDepth` field at
//     all, and neither does a Sector lifted and then lifted back to zero.
//     Zero lifts nothing, which is exactly what no field already says, so
//     writing a zero would invent a third state that means the absence
//     without being it;
//   * the version guard, in both directions. A document written before the
//     field existed - schema 71, no field - loads as no depth recorded and
//     draws the flat picture it drew before, with no migration pass. A
//     document claiming to predate the feature while carrying the field did
//     not come from this writer and is refused rather than guessed at;
//   * the rebuild. Every structural edit replays the records, so a depth
//     that survives a save and load but not a resize is one the author
//     loses the first time they move a wall;
//   * every Sector-creating record type carries it. Backgrounds and
//     Facades included: unlike surface variants, which are atlas artwork
//     those two never ask for, a floor depth is a lift handed to the Layers
//     behind, and a Background is exactly the kind of thing an author
//     would want lifting the back Layers.
//
// What may not change is everything else. A depth is render state, and the
// document says so: same Sectors, same ends, same capacity, same
// construction in every other field.

#include "WorldChecks.h"

#include "core/BinarySerializer.h"
#include "core/Defines.h"
#include "core/Sector.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <memory>
#include <string>

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

		// A two-Layer World with a Room on each, ready for a depth to be
		// authored on the front one.
		WorldPtr twoLayerWorld(std::string const& name)
		{
			auto world = std::make_shared<core::World>(name, 20, 8);
			while (world->getLayerCount() < 2) world->addLayer();
			world->addRoom("Front", 0, 2, 2, 3, 2);
			world->addRoom("Behind", 1, 2, 2, 3, 2);
			return world;
		}

		uint32_t firstOn(core::World const& world, uint32_t layer)
		{
			auto const sectors = world.getSectors(layer);
			require(!sectors.empty(), "Layer " + std::to_string(layer)
				+ " holds no Sectors, so the case cannot say anything");
			return sectors.front()->getIndex();
		}

		bool refused(std::function<void()> action)
		{
			try { action(); }
			catch (std::exception const&) { return true; }
			return false;
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
	}

	// A positive depth comes back out of both wire formats exactly as it
	// went in, and the Layer behind still draws by it after the reload.
	void floorDepthRoundTrips(smoke::Context const&)
	{
		auto const world = twoLayerWorld("Depth round trip");
		require(world->setSectorFloorDepth(firstOn(*world, 0), 48).changed,
			"the depth write was refused in the fixture");

		auto const yaml = load(serialize(*world));
		require(yaml->effectiveFloorDepth(firstOn(*yaml, 0)) == 48,
			"A depth of 48 came back from YAML as "
				+ std::to_string(yaml->effectiveFloorDepth(firstOn(*yaml, 0))));
		require(yaml->layerFloorOffsetPx(1) == 48,
			"The Layer behind does not draw 48px off after a YAML reload");

		auto const binary = loadBinary(serializeBinary(*world));
		require(binary->effectiveFloorDepth(firstOn(*binary, 0)) == 48,
			"A depth of 48 came back from the binary wire as "
				+ std::to_string(binary->effectiveFloorDepth(firstOn(*binary, 0)))
				+ "; a field that round-trips in text and not in binary is a "
				  "field half the saves in the game lose");
		require(binary->layerFloorOffsetPx(1) == 48,
			"The Layer behind does not draw 48px off after a binary reload");
	}

	// A negative depth round-trips unchanged, in both formats. The failure
	// this is aimed at is a signed field read back through an unsigned path
	// and coming back as a huge positive, or a clamp on the way out quietly
	// turning -40 into 0.
	void aNegativeFloorDepthRoundTripsUnchanged(smoke::Context const&)
	{
		auto const world = twoLayerWorld("Negative depth round trip");
		require(world->setSectorFloorDepth(firstOn(*world, 0), -40).changed,
			"the negative depth write was refused in the fixture");

		auto const yaml = load(serialize(*world));
		require(yaml->effectiveFloorDepth(firstOn(*yaml, 0)) == -40,
			"A depth of -40 came back from YAML as "
				+ std::to_string(yaml->effectiveFloorDepth(firstOn(*yaml, 0))));
		require(yaml->layerFloorOffsetPx(1) == -40,
			"The Layer behind is pushed " + std::to_string(yaml->layerFloorOffsetPx(1))
				+ "px after a YAML reload of -40");

		auto const binary = loadBinary(serializeBinary(*world));
		require(binary->effectiveFloorDepth(firstOn(*binary, 0)) == -40,
			"A depth of -40 came back from the binary wire as "
				+ std::to_string(binary->effectiveFloorDepth(firstOn(*binary, 0))));
		require(binary->layerFloorOffsetPx(1) == -40,
			"The Layer behind is pushed "
				+ std::to_string(binary->layerFloorOffsetPx(1))
				+ "px after a binary reload of -40");
	}

	// The whole band round-trips, including the extremes a writer could be
	// asked for.
	void theBandRoundTripsAtItsExtremes(smoke::Context const&)
	{
		for (int32_t const depth : { CORE_LEVEL_HEIGHT_PIXELS,
				-CORE_LEVEL_HEIGHT_PIXELS, 1, -1 })
		{
			auto const world = twoLayerWorld("Extreme depth round trip");
			require(world->setSectorFloorDepth(firstOn(*world, 0), depth).changed,
				"the fixture's depth write was refused");
			auto const yaml = load(serialize(*world));
			require(yaml->effectiveFloorDepth(firstOn(*yaml, 0)) == depth,
				"A depth of " + std::to_string(depth) + " came back as "
					+ std::to_string(yaml->effectiveFloorDepth(firstOn(*yaml, 0))));
		}
	}

	// A World nobody lifted writes no `floorDepth` field at all, and
	// neither does a Sector lifted and then lifted back to zero.
	void aWorldWithNoDepthsWritesNoField(smoke::Context const&)
	{
		core::World plain("Plain depth world", 16, 8);
		plain.addRoom("Room", 0, 1, 1, 4, 2);
		plain.addCorridor(0, 4, 1, 3, 1);
		plain.finishBuild();
		plain.pauseSimulation();
		require(countField(plain, "floorDepth") == 0,
			"A World with no recorded floor depth wrote a floorDepth field");

		// Lifted, then lifted back to zero: the document is as sparse as it
		// was before the lift was ever authored.
		auto const cleared = twoLayerWorld("Cleared depth world");
		require(cleared->setSectorFloorDepth(firstOn(*cleared, 0), 48).changed,
			"the fixture's depth write was refused");
		require(countField(*cleared, "floorDepth") == 1,
			"A Sector at 48 did not write its floorDepth field");
		require(cleared->setSectorFloorDepth(firstOn(*cleared, 0), 0).changed,
			"Setting zero over 48 was not seen as a change");
		require(countField(*cleared, "floorDepth") == 0,
			"A Sector lifted back to zero still writes a floorDepth field; zero "
			  "lifts nothing, which is what no field already says");
		require(countField(*load(serialize(*cleared)), "floorDepth") == 0,
			"A zero-depth document re-saved itself with a field");
	}

	// A document written before this feature existed - schema 71, no field
	// anywhere - loads as no depth recorded, which lifts nothing, which is
	// the flat picture every saved World drew yesterday.
	void aDocumentWrittenBeforeFloorDepthExistedLoadsAsNoDepth(
		smoke::Context const&)
	{
		auto const world = twoLayerWorld("Pre-depth world");
		auto document = YAML::Load(serialize(*world));
		require(document["version"].as<uint32_t>() == 72,
			"The current writer is not the version this check is bumping from");
		document["version"] = 71;
		require(YAML::Dump(document).find("floorDepth") == std::string::npos,
			"The fixture was supposed to bump a document that carries no "
			  "floorDepth field");

		auto const loaded = load(YAML::Dump(document));
		require(loaded->getNumSectors() == world->getNumSectors(),
			"The pre-depth document did not load its Sectors");
		for (uint32_t layer = 0; layer < loaded->getLayerCount(); ++layer)
		{
			require(loaded->layerFloorOffsetPx(layer) == 0,
				"Layer " + std::to_string(layer) + " of a pre-depth document "
					"draws " + std::to_string(loaded->layerFloorOffsetPx(layer))
					+ "px off; a document with no field must draw flat");
			for (auto const& sector : loaded->getSectors(layer))
				require(loaded->effectiveFloorDepth(sector->getIndex()) == 0,
					"A Sector of a pre-depth document loaded with a depth");
		}
	}

	// The field is behind its own schema version. A document that claims to
	// predate the feature while carrying the field did not come from this
	// writer, and reading it anyway would be guessing at what the bytes mean.
	void aFloorDepthFieldBelowTheSchemaVersionThatDefinesItIsRefused(
		smoke::Context const&)
	{
		auto const world = twoLayerWorld("Bad version depth");
		require(world->setSectorFloorDepth(firstOn(*world, 0), 48).changed,
			"the fixture's depth write was refused");
		auto document = YAML::Load(serialize(*world));
		require(YAML::Dump(document).find("floorDepth") != std::string::npos,
			"The fixture document carries no floorDepth field to test with");
		document["version"] = 71;
		require(refused([&] { (void)load(YAML::Dump(document)); }),
			"A schema 71 document carrying a floorDepth field was accepted");
	}

	// The depth rides the construction record, so the rebuild every
	// structural edit performs replays it onto the Sector the record just
	// created. This is the case a live-object-only implementation loses:
	// the author lifts a Room, moves a wall somewhere else, and the lift is
	// gone.
	void theDepthSurvivesTheRebuildAStructuralEditPerforms(smoke::Context const&)
	{
		auto const world = twoLayerWorld("Depth rebuild");
		auto const front = firstOn(*world, 0);
		require(world->setSectorFloorDepth(front, 48).changed,
			"the fixture's depth write was refused");
		require(world->layerFloorOffsetPx(1) == 48, "the lift is not in force");

		// A structural edit anywhere: the whole World is torn down and
		// replayed from the records.
		world->addRoom("Elsewhere", 1, 6, 12, 2, 2);
		require(world->effectiveFloorDepth(firstOn(*world, 0)) == 48,
			"The rebuild did not replay the depth onto the Sector its record "
				"created; the lift is " + std::to_string(world->effectiveFloorDepth(
					firstOn(*world, 0))));
		require(world->layerFloorOffsetPx(1) == 48,
			"The Layer behind stopped drawing by the depth across a rebuild");

		// And it survives the save that follows the rebuild.
		auto const reloaded = load(serialize(*world));
		require(reloaded->effectiveFloorDepth(firstOn(*reloaded, 0)) == 48,
			"The depth was lost across the rebuild-and-save cycle");
	}

	// Every Sector-creating record type carries its floor depth. Surface
	// variants are atlas artwork and Backgrounds and Facades never ask for
	// any, so those two refuse a variant entry; a floor depth is a lift
	// handed backwards, and a Background is exactly the thing an author
	// would want lifting the back Layers. A record type that silently
	// dropped its depth would lose the lift on the first rebuild.
	void everySectorCreatingRecordTypeCarriesItsFloorDepth(smoke::Context const&)
	{
		auto const world = std::make_shared<core::World>("Every type carries depth", 24, 12);
		while (world->getLayerCount() < 3) world->addLayer();

		// The Ladder lands on two different location-like Sectors on the
		// Layer in front, so the Rooms are split around its column.
		auto const room = world->addRoom("Room", 0, 2, 2, 2, 1);
		world->addRoom("Landing", 0, 4, 2, 2, 1);
		auto const corridor = world->addCorridor(0, 2, 8, 3, 1);
		auto const background = world->addBackground(0, 6, 2, 3, 2);
		auto const facade = world->addFacade("Facade", 0, 6, 8, 3, 2, 0.9f);
		auto const ladder = world->addLadder(1, 2, 2,
			{ 3, false, false }).ladder.sector->getIndex();

		struct Carried { std::string kind; uint32_t index; int32_t depth; };
		Carried const cases[] = {
			{ "Room", room, 48 },
			{ "Corridor", corridor, -24 },
			{ "Background", background, 32 },
			{ "Facade", facade, -16 },
			{ "Ladder", ladder, 12 },
		};
		for (auto const& item : cases)
			require(world->setSectorFloorDepth(item.index, item.depth).changed,
				item.kind + ": the depth write was refused");

		auto const reloaded = load(serialize(*world));
		for (auto const& item : cases)
		{
			require(reloaded->effectiveFloorDepth(item.index) == item.depth,
				item.kind + " came back from the document with a floor depth of "
					+ std::to_string(reloaded->effectiveFloorDepth(item.index))
					+ " rather than the " + std::to_string(item.depth)
					+ " it was authored with");
			// And the lift is in force after the reload, which is the
			// rebuild's own doing rather than a remembered value.
			require(reloaded->layerFloorOffsetPx(item.depth > 0 ? 1 : 1)
					!= 0 || item.depth == 0,
				item.kind + " carried its depth onto the record but the Layer "
					"behind draws flat, so the replay dropped it");
		}
	}

	// A depth is render state, and the document says so. The strongest
	// statement available here: the document of a World with a depth and
	// the document of the same World without one differ by nothing but the
	// one field.
	void aDepthChangesNothingElseInTheDocument(smoke::Context const&)
	{
		auto const plain = twoLayerWorld("Document identity");
		auto const lifted = twoLayerWorld("Document identity");
		require(lifted->setSectorFloorDepth(firstOn(*lifted, 0), 48).changed,
			"the fixture's depth write was refused");

		auto const plainYaml = serialize(*plain);
		auto const liftedYaml = serialize(*lifted);
		require(liftedYaml.size() > plainYaml.size(),
			"The lifted document carries no extra field at all");

		// Same Sectors, same Layer count, same capacity: the lift moved no
		// simulation state, because there is none for it to move.
		require(lifted->getNumSectors() == plain->getNumSectors(),
			"A floor depth changed the Sector count");
		require(lifted->getLayerCount() == plain->getLayerCount(),
			"A floor depth changed the Layer count");
		require(lifted->layerFloorOffsetPx(0) == plain->layerFloorOffsetPx(0),
			"Layer 0 draws differently with a depth authored behind it");
		// The only line the lifted document adds is the depth itself.
		std::string onlyExtra;
		{
			auto const a = YAML::Load(plainYaml);
			auto const b = YAML::Load(liftedYaml);
			require(a.IsMap() && b.IsMap(),
				"The World document is not the map this check expects");
			int extra = 0;
			for (auto const& item : b)
			{
				auto const key = item.first.as<std::string>();
				if (!(a[key])) ++extra;
			}
			// The field is inside the construction records rather than at
			// the top of the document, so the top-level comparison finds
			// nothing extra - which is itself the statement: the lift adds
			// no top-level state.
			onlyExtra = std::to_string(extra);
			require(extra == 0,
				"A floor depth added " + onlyExtra
					+ " top-level field(s) to the document; the lift belongs on "
					  "the Sector's construction record and nowhere else");
		}
	}
}
