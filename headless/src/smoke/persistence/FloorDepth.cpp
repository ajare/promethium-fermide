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
//   * sparseness, and where #1112 put the boundary. A World nobody lifted
//     writes no `floorDepth` field at all, and that is what keeps every
//     World written before the feature existed byte-for-byte what it was.
//     A Sector pinned to zero does write its zero: the presence of the
//     field is the override and the value is the depth, so the zero is a
//     fact the document must carry rather than a third way of saying the
//     absence. Clearing is what removes the field;
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

	// A World nobody lifted writes no `floorDepth` field at all. A Sector
	// pinned to zero does write one, and that is the #1112 change: the zero
	// is not the absence wearing a different face, it is a fact the
	// document has to carry - this Sector stays unlifted even while its
	// type lifts - and a reader that never sees it cannot put the pin back.
	// Only clearing removes the field.
	void aWorldWithNoDepthsWritesNoField(smoke::Context const&)
	{
		core::World plain("Plain depth world", 16, 8);
		plain.addRoom("Room", 0, 1, 1, 4, 2);
		plain.addCorridor(0, 4, 1, 3, 1);
		plain.finishBuild();
		plain.pauseSimulation();
		require(countField(plain, "floorDepth") == 0,
			"A World with no recorded floor depth wrote a floorDepth field");

		// Lifted, then pinned to zero: the field stays, now carrying the
		// zero, because the pin is a thing the author said.
		auto const pinned = twoLayerWorld("Pinned depth world");
		require(pinned->setSectorFloorDepth(firstOn(*pinned, 0), 48).changed,
			"the fixture's depth write was refused");
		require(countField(*pinned, "floorDepth") == 1,
			"A Sector at 48 did not write its floorDepth field");
		require(pinned->setSectorFloorDepth(firstOn(*pinned, 0), 0).changed,
			"Setting zero over 48 was not seen as a change");
		require(countField(*pinned, "floorDepth") == 1,
			"A Sector pinned to zero wrote no floorDepth field; the zero is the "
			"pin, and a document that drops it cannot restore it (#1112)");

		// And the pin comes back off the document rather than being inferred.
		auto const reloaded = load(serialize(*pinned));
		require(reloaded->hasFloorDepthOverride(firstOn(*reloaded, 0)),
			"A zero override came back from the document as no override at all");
		require(reloaded->effectiveFloorDepth(firstOn(*reloaded, 0)) == 0,
			"A pinned Sector came back answering "
				+ std::to_string(reloaded->effectiveFloorDepth(firstOn(*reloaded, 0))));
		require(countField(*reloaded, "floorDepth") == 1,
			"A zero-pinned document re-saved itself with the field lost, which "
			"silently un-pins the Sector on the next save");

		// Clearing is the only thing that takes the field back off.
		auto const clearWrite = pinned->clearSectorFloorDepth(firstOn(*pinned, 0));
		require(clearWrite.accepted && clearWrite.changed,
			"clearing the pin was not an accepted write that changed anything");
		require(countField(*pinned, "floorDepth") == 0,
			"Clearing the override left the floorDepth field in the document");
	}

	// A document written before this feature existed - schema 71, no field
	// anywhere - loads as no depth recorded, which lifts nothing, which is
	// the flat picture every saved World drew yesterday.
	void aDocumentWrittenBeforeFloorDepthExistedLoadsAsNoDepth(
		smoke::Context const&)
	{
		auto const world = twoLayerWorld("Pre-depth world");
		auto document = YAML::Load(serialize(*world));
		require(document["version"].as<uint32_t>() == 74,
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

	// The ticket's own case on the document (#1112). The author lifts a
	// whole Sector type and holds one Room back down inside it: the type
	// says 24, this Sector says 0, and the document has to carry both
	// opinions so a reload puts them both back. A writer that dropped the
	// zero as "the same as nothing" would hand the reloaded World a Sector
	// with no override, and the type default would lift it 24px - the
	// author's pin silently released on the save.
	void aZeroOverrideAgainstALiftedTypeRoundTrips(smoke::Context const&)
	{
		auto const world = twoLayerWorld("Pinned against a lifted type");
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"the fixture's type default was refused");
		auto const pin = world->setSectorFloorDepth(firstOn(*world, 0), 0);
		require(pin.accepted && pin.changed,
			"pinning the front Sector to zero was not an accepted write that "
			"changed anything");

		// In force before any save: the pin wins over the type, and the
		// Layer behind draws nothing from a Sector that lifts nothing.
		require(world->effectiveFloorDepth(firstOn(*world, 0)) == 0,
			"A Sector pinned to zero over a type at 24 answers "
				+ std::to_string(world->effectiveFloorDepth(firstOn(*world, 0))));
		require(world->layerFloorOffsetPx(1) == 0,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
				+ "px off; the pinned front Sector lifts nothing, whatever its "
				  "type says");
		require(world->effectiveFloorDepth(firstOn(*world, 1)) == 24,
			"A Sector left following the type answers "
				+ std::to_string(world->effectiveFloorDepth(firstOn(*world, 1)))
				+ " rather than the type's 24");

		// Both wire formats, and the pin comes back as a pin rather than as
		// the absence the type would fill.
		for (bool const binary : { false, true })
		{
			auto const reloaded = binary
				? loadBinary(serializeBinary(*world))
				: load(serialize(*world));
			char const* const kind = binary ? "binary" : "YAML";
			require(reloaded->hasFloorDepthOverride(firstOn(*reloaded, 0)),
				std::string("A zero override came back from ") + kind
					+ " as no override, which puts the Sector back on a type that "
					  "lifts it (#1112)");
			require(reloaded->effectiveFloorDepth(firstOn(*reloaded, 0)) == 0,
				std::string("The pinned Sector came back from ") + kind
					+ " answering "
					+ std::to_string(reloaded->effectiveFloorDepth(firstOn(*reloaded, 0))));
			require(reloaded->layerFloorOffsetPx(1) == 0,
				std::string("Layer 1 draws ")
					+ std::to_string(reloaded->layerFloorOffsetPx(1))
					+ "px off after a " + kind
					+ " reload of a World whose front Sector is pinned to zero");
			// The type default is untouched by the pin and still carries the
			// Sector that never overrode it.
			require(reloaded->sectorTypeFloorDepth(core::SectorType::Location) == 24,
				std::string("The type default came back from ") + kind + " as "
					+ std::to_string(reloaded->sectorTypeFloorDepth(
						core::SectorType::Location)));
			require(reloaded->effectiveFloorDepth(firstOn(*reloaded, 1)) == 24,
				std::string("A Sector following the lifted type came back from ")
					+ kind + " answering "
					+ std::to_string(reloaded->effectiveFloorDepth(
						firstOn(*reloaded, 1))));
		}
	}

	// A document claiming to predate #1112 while carrying a zero depth did
	// not come from this writer: the pre-74 writer never emitted a zero,
	// because it stored zero as nothing. Reading one anyway would be
	// guessing at what the bytes mean, and the guess would be the one the
	// version bump exists to prevent.
	void aZeroDepthFieldBelowVersion74IsRefused(smoke::Context const&)
	{
		auto const world = twoLayerWorld("Zero below the version that allows it");
		auto const pin = world->setSectorFloorDepth(firstOn(*world, 0), 0);
		require(pin.accepted && pin.changed,
			"pinning the front Sector to zero was not an accepted write that "
			"changed anything, so the fixture has no zero field to test with");
		auto document = YAML::Load(serialize(*world));
		require(document["construction"] && YAML::Dump(document).find("floorDepth: 0")
				!= std::string::npos,
			"The fixture document carries no zero floorDepth field to test with");
		document["version"] = 73;
		require(refused([&] { (void)load(YAML::Dump(document)); }),
			"A schema 73 document carrying a zero floorDepth was accepted; the "
			  "pre-74 writer never emitted one, so this document is not what it "
			  "claims to be (#1112)");

		// The same document at its own version loads, which is what makes the
		// refusal about the version and not about the field.
		auto const allowed = YAML::Load(serialize(*world));
		require(allowed["version"].as<uint32_t>() >= 74,
			"The current writer is below the version that carries a zero depth");
		require(!refused([&] { (void)load(YAML::Dump(allowed)); }),
			"The same zero override was refused at its own schema version");
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

	// The zero pin survives the rebuild every structural edit performs,
	// which is the case a live-object-only implementation loses twice over:
	// a Sector whose zero was only ever set on the live object comes back
	// from the replay with no override, and the type default lifts it. The
	// author pins a Room, moves a wall somewhere else, and the Room jumps.
	void aZeroOverrideSurvivesTheRebuildAStructuralEditPerforms(
		smoke::Context const&)
	{
		auto const world = twoLayerWorld("Zero pin rebuild");
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"the fixture's type default was refused");
		auto const front = firstOn(*world, 0);
		auto const pin = world->setSectorFloorDepth(front, 0);
		require(pin.accepted && pin.changed,
			"pinning the front Sector to zero was not an accepted write that "
			"changed anything");
		require(world->layerFloorOffsetPx(1) == 0,
			"the pin is not in force before the edit");

		// A structural edit somewhere else entirely: the whole World is torn
		// down and replayed from the records.
		world->addRoom("Elsewhere", 1, 6, 12, 2, 2);
		require(world->hasFloorDepthOverride(firstOn(*world, 0)),
			"The rebuild replayed the front Sector without its zero override, so "
			  "the type default has it again (#1112)");
		require(world->effectiveFloorDepth(firstOn(*world, 0)) == 0,
			"After the rebuild the pinned Sector answers "
				+ std::to_string(world->effectiveFloorDepth(firstOn(*world, 0)))
				+ " rather than the zero it was pinned with");
		require(world->layerFloorOffsetPx(1) == 0,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
				+ "px off after the rebuild; the pin released across it");
		require(world->effectiveFloorDepth(firstOn(*world, 1)) == 24,
			"The Sector that follows the type answers "
				+ std::to_string(world->effectiveFloorDepth(firstOn(*world, 1)))
				+ " after the rebuild");

		// And the save that follows the rebuild still carries the pin, rather
		// than the flattened answer the replay would have written had the
		// override been lost on the way.
		auto const reloaded = load(serialize(*world));
		require(reloaded->hasFloorDepthOverride(firstOn(*reloaded, 0)),
			"The zero pin was lost across the rebuild-and-save cycle");
		require(reloaded->effectiveFloorDepth(firstOn(*reloaded, 0)) == 0,
			"The rebuilt-and-reloaded Sector answers "
				+ std::to_string(reloaded->effectiveFloorDepth(firstOn(*reloaded, 0))));
		require(reloaded->layerFloorOffsetPx(1) == 0,
			"Layer 1 draws " + std::to_string(reloaded->layerFloorOffsetPx(1))
				+ "px off after the rebuild-and-save cycle");
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
