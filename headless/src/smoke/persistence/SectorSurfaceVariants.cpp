// A World records Sector surface variants and keeps them (#1040).
//
// The entries are authored on the Sector's own construction record, in
// Sector-local column and level, and the live Sector carries the same list
// so the renderer can ask it per cell. Both halves matter, and each has a
// distinct way to go wrong:
//
//   * held only on the live Sector, the entries evaporate on the first
//     structural edit, because every structural edit tears the World down
//     and replays the records;
//   * held only on the record, nothing can draw them without walking the
//     record list per cell;
//   * written into every record whether used or not, every World that never
//     heard of variants starts carrying a field it never asked for.
//
// So the checks here are the round trip, the absence, and the rebuild. A
// document written before this feature existed - schema 69, no field -
// loads as no variants recorded. A World that never used variants saves
// exactly as it did before, with no empty field. A World that did use them
// saves them, reloads them, and survives a resize with them still pointing
// at the cells they were authored against, because Sector-local
// coordinates do not care where the Sector ended up.
//
// What may not change is everything else. The strongest statement
// available here is that a World's document with variants and the same
// World's document without them differ by nothing but the variant block:
// same sectors, same ends, same capacity, same construction in every other
// field. Variants are artwork, and the document says so.

#include "WorldChecks.h"

#include "core/Sector.h"
#include "core/SectorSurfaceVariant.h"
#include "core/BinarySerializer.h"
#include "core/ChamberSubtype.h"
#include "core/Defines.h"
#include "core/Exceptions.h"
#include "core/SerializationException.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

#include <yaml-cpp/yaml.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace persistence
{
	using smoke::require;

	namespace
	{
		using Entries = std::vector<core::SectorSurfaceVariant>;
		using WorldPtr = std::shared_ptr<core::World>;

		std::string serialize(core::World const& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		}

		// The binary wire carries the same records, so the entries have to
		// survive it as well as the text form. A field that round-trips in
		// YAML and not in binary is a field half the saves in the game lose.
		std::string serializeBinary(core::World const& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::BinarySerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		}

		WorldPtr loadBinary(std::string const& bytes)
		{
			auto loaded = std::make_shared<core::World>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::BinarySerializer::fromString(bytes);
			reader->deserialize();
			require(loaded->deserialize(*reader, workData),
				"The binary World document did not load");
			return loaded;
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

		// Writes a `surfaceVariants` block into the document's Room record
		// the way a hand-authored World file carries one. No C++ authoring
		// call is involved: the ticket is that a World file can say this, so
		// this is the shape under test.
		std::string withVariants(std::string const& yaml, Entries const& entries)
		{
			if (entries.empty()) return yaml;
			auto document = YAML::Load(yaml);
			require(document.IsMap() && document["construction"].IsSequence(),
				"The serialized World has no construction list to author on");
			bool authored = false;
			for (auto record : document["construction"])
			{
				if (!record.IsMap() || record["type"].as<std::string>() != "room")
					continue;
				YAML::Node variants;
				for (auto const& entry : entries)
				{
					YAML::Node item;
					item["column"] = entry.column;
					item["level"] = entry.level;
					item["variant"] = entry.variant;
					variants.push_back(item);
				}
				record["surfaceVariants"] = variants;
				authored = true;
			}
			require(authored, "The serialized World carried no Room record");
			return YAML::Dump(document);
		}

		// The same document with every `surfaceVariants` block taken back
		// out. Dumped through the same node round trip as `withVariants`, so
		// two documents that differ only by that block compare equal here.
		std::string withoutVariants(std::string const& yaml)
		{
			auto document = YAML::Load(yaml);
			for (auto record : document["construction"])
				if (record.IsMap() && record["surfaceVariants"])
					record.remove("surfaceVariants");
			return YAML::Dump(document);
		}

		std::shared_ptr<const core::Sector> room(core::World const& world,
			uint32_t index)
		{
			return index < world.getNumSectors() ? world.getSector(index) : nullptr;
		}

		Entries sampleEntries()
		{
			return { { 0, 0, 1 }, { 2, 1, 2 }, { 3, 0, 7 } };
		}

		// A World with one 4x2 Room, saved, authored with the given entries
		// and loaded back. The entry with variant 7 is deliberate: nothing
		// anywhere refuses a number the artwork has no row for, because the
		// renderer answers that, not the document.
		WorldPtr authoredRoomWorld(Entries const& entries)
		{
			core::World plain("Variant room world", 12, 8);
			plain.addRoom("Room", 0, 1, 1, 4, 2);
			plain.finishBuild();
			plain.pauseSimulation();
			return load(withVariants(serialize(plain), entries));
		}

		bool refused(std::function<void()> action)
		{
			try { action(); }
			catch (core::SerializationException const&) { return true; }
			catch (core::WorldException const&) { return true; }
			catch (std::exception const&) { return true; }
			return false;
		}

		// Puts a variant block on every record of one type. Used to author a
		// variant onto a transport's own creation record, which is the case
		// that exercises the wiring from a record to the Sector it created -
		// a Room's index is handed straight back, a transport's comes out of
		// the creating call's result.
		void setRecordVariants(YAML::Node& document, std::string const& type,
			Entries const& entries)
		{
			YAML::Node variants;
			for (auto const& entry : entries)
			{
				YAML::Node item;
				item["column"] = entry.column;
				item["level"] = entry.level;
				item["variant"] = entry.variant;
				variants.push_back(item);
			}
			bool found = false;
			for (auto record : document["construction"])
				if (record.IsMap() && record["type"].as<std::string>() == type)
				{
					record["surfaceVariants"] = variants;
					found = true;
				}
			require(found, "The fixture document carried no " + type + " record");
		}

		std::shared_ptr<const core::Sector> sectorNamed(core::World const& world,
			std::string const& name)
		{
			for (uint32_t index = 0; index < world.getNumSectors(); ++index)
				if (world.getSector(index)->getName() == name) return world.getSector(index);
			return nullptr;
		}

		// The refusal's text, not only that a refusal happened. The message is
		// the author's one notice of which cell was miscounted, so the checks
		// that care about it assert it names the column and level rather than
		// merely that something threw.
		bool refusedWith(std::function<void()> action, std::string* reason)
		{
			try { action(); }
			catch (core::SerializationException const& e) { *reason = e.what(); return true; }
			catch (core::WorldException const& e) { *reason = e.what(); return true; }
			catch (std::exception const& e) { *reason = e.what(); return true; }
			reason->clear();
			return false;
		}

		// The smallest World whose saved document carries one record of one
		// type, for every record type that can carry variants. Each type gets
		// its own fixture so a refusal for one of them cannot be hiding a
		// record that was never there to refuse, and so the same refusal is
		// seen arriving down ten different replay paths rather than one.
		struct TypeFixture
		{
			char const* type;
			std::function<void(core::World&)> build;
		};

		std::vector<TypeFixture> variantCarryingFixtures()
		{
			return {
				{ "corridor", [](core::World& w) { w.addCorridor(0, 1, 1, 4, 1); } },
				{ "room", [](core::World& w) { w.addRoom("Room", 0, 1, 1, 4, 2); } },
				{ "ladder", [](core::World& w)
					{
						w.addCorridor(0, 0, 12); w.addCorridor(3, 0, 12);
						w.addLadder(1, 0, 2, { 4, false, false });
					} },
				{ "stairwell", [](core::World& w)
					{
						w.addCorridor(0, 0, 12); w.addCorridor(1, 0, 12);
						w.addStairwell(1, 0, 2, 2, CORE_SIDE_LEFT);
					} },
				{ "staircase", [](core::World& w)
					{
						w.addCorridor(0, 0, 12); w.addCorridor(1, 0, 12);
						w.addStaircase(1, 0, 4, { 3, CORE_SIDE_RIGHT, 0.f });
					} },
				{ "lift", [](core::World& w)
					{
						w.addCorridor(0, 0, 12); w.addCorridor(3, 0, 12);
						core::World::CreateLiftOptions options;
						options.cellsWide = 1;
						options.stopOffsets = { 0, 3 };
						options.capacity = 2;
						w.addLift(1, 0, 6, options);
					} },
				{ "shuttle", [](core::World& w)
					{
						w.addCorridor(0, 0, 40); w.addCorridor(2, 0, 40);
						core::World::CreateShuttleOptions options;
						options.numCars = 1;
						options.carWidth = 4;
						options.stopOffsets = { 0, 24 };
						options.initialStop = 0;
						w.addShuttle(1, 0, 4, 36, options);
					} },
				{ "airlock", [](core::World& w)
					{
						w.addRoom("Left", 0, 0, 0, 4, 1);
						w.addRoom("Right", 0, 0, 6, 4, 1);
						w.addAirlock(0, 0, 4, 2, 1.f);
					} },
				{ "chamber", [](core::World& w)
					{
						w.addRoom("Left", 0, 0, 0, 4, 1);
						w.addRoom("Right", 0, 0, 6, 4, 1);
						w.addChamber(0, 0, 4, 2, true, core::ChamberSubtype::Decontamination);
					} },
				{ "dumbwaiter", [](core::World& w)
					{
						w.addCorridor(0, 0, 8); w.addCorridor(1, 0, 8);
						w.addDumbwaiter(1, 0, 3);
					} },
			};
		}

		// One entry authored onto the fixture's own record type, at its first
		// cell, which every one of these Sectors has.
		std::string authoredOn(TypeFixture const& fixture, uint32_t variant)
		{
			core::World world("Variant refusal fixture", 48, 12);
			fixture.build(world);
			world.finishBuild();
			world.pauseSimulation();
			auto document = YAML::Load(serialize(world));
			setRecordVariants(document, fixture.type, { { 0, 0, variant } });
			return YAML::Dump(document);
		}

		bool anySectorWears(WorldPtr const& world, Entries const& entries)
		{
			for (uint32_t index = 0; index < world->getNumSectors(); ++index)
				if (world->getSector(index)->getSurfaceVariants() == entries) return true;
			return false;
		}

		// A binary document recording one variant number on a Room's cell 2,1.
		std::string binaryRecordingVariant(uint32_t variant)
		{
			core::World world("Binary variant fixture", 12, 8);
			world.addRoom("Room", 0, 1, 1, 4, 2);
			world.finishBuild();
			world.pauseSimulation();
			std::string diagnostic;
			require(world.setSectorSurfaceVariant(0, 2, 1, variant, &diagnostic).accepted,
				"The authoring seam refused the binary fixture's own variant: " + diagnostic);
			return serializeBinary(world);
		}

		// The same document with that number set to zero, made by patching the
		// one byte the variant occupies rather than by hand-writing the wire
		// format. The patch is checked against the two documents it came from -
		// equal length, exactly one differing byte, holding 1 in one and 2 in
		// the other - so a wire format that moved the number fails this check
		// loudly instead of quietly authoring nothing at all.
		std::string binaryRecordingVariantZero()
		{
			auto const one = binaryRecordingVariant(1);
			auto const two = binaryRecordingVariant(2);
			require(one.size() == two.size(),
				"The two binary documents that differ only by a variant number "
				"differ in length, so the patched one is not what it claims to be");
			std::vector<size_t> differing;
			for (size_t offset = 0; offset < one.size(); ++offset)
				if (one[offset] != two[offset]) differing.push_back(offset);
			require(differing.size() == 1,
				"The variant number is not the one byte this patch aims at: the two "
				"documents differ at " + std::to_string(differing.size()) + " offsets");
			require(static_cast<uint8_t>(one[differing[0]]) == 1
					&& static_cast<uint8_t>(two[differing[0]]) == 2,
				"The byte this patch overwrites does not hold the variant numbers 1 and 2");
			auto zero = one;
			zero[differing[0]] = '\0';
			return zero;
		}
	}

	// Save, reload, entries intact.
	void sectorSurfaceVariantsRoundTrip(smoke::Context const&)
	{
		auto const entries = sampleEntries();
		auto const original = authoredRoomWorld(entries);
		require(original->getNumSectors() >= 1, "The authored World has no Sector");
		auto const originalRoom = room(*original, 0);
		require(originalRoom != nullptr, "The authored World has no Room");
		require(originalRoom->getSurfaceVariants() == entries,
			"The loaded Room does not carry the entries the document recorded");
		require(originalRoom->surfaceVariantAt(0, 0)
				== std::optional<uint32_t>{ 1 },
			"The loaded Room does not answer for column 0, level 0");
		require(originalRoom->surfaceVariantAt(2, 1)
				== std::optional<uint32_t>{ 2 },
			"The loaded Room does not answer for column 2, level 1");
		require(originalRoom->surfaceVariantAt(3, 0)
				== std::optional<uint32_t>{ 7 },
			"A variant number with no artwork behind it was not retained");
		require(!originalRoom->surfaceVariantAt(1, 1),
			"A cell with no entry answered as though it had one");

		// And again: saving the loaded World writes the same block back, so
		// the entries survive a second round trip rather than decaying.
		auto const resaved = serialize(*original);
		require(resaved.find("surfaceVariants") != std::string::npos
				&& resaved.find("column:") != std::string::npos
				&& resaved.find("level:") != std::string::npos
				&& resaved.find("variant:") != std::string::npos,
			"The re-saved document did not persist the variant entries");
		auto const again = load(resaved);
		require(room(*again, 0)->getSurfaceVariants() == entries,
			"The entries did not survive a second save and reload");

		// And through the binary form of the same document.
		auto const binary = loadBinary(serializeBinary(*original));
		require(room(*binary, 0)->getSurfaceVariants() == entries,
			"The entries did not survive the binary round trip");
		require(room(*binary, 0)->surfaceVariantAt(2, 1)
				== std::optional<uint32_t>{ 2 },
			"The binary round trip lost an entry's cell or variant number");
	}

	// A World that never used variants saves as it did before, with no
	// empty field written.
	void aWorldThatNeverUsedVariantsWritesNoField(smoke::Context const&)
	{
		core::World plain("Plain room world", 12, 8);
		plain.addRoom("Room", 0, 1, 1, 4, 2);
		plain.addCorridor(0, 4, 1, 3, 1);
		plain.finishBuild();
		plain.pauseSimulation();
		auto const yaml = serialize(plain);
		require(yaml.find("surfaceVariants") == std::string::npos,
			"A World with no recorded variants wrote a surfaceVariants field");
		require(yaml.find("variant:") == std::string::npos,
			"A World with no recorded variants wrote variant entries anyway");
		auto const loaded = load(yaml);
		for (uint32_t index = 0; index < loaded->getNumSectors(); ++index)
			require(room(*loaded, index)->getSurfaceVariants().empty(),
				"A Sector that was never given a variant loaded with entries");
	}

	// A file written before this feature existed loads as no variants
	// recorded. Schema 69 is the last version that carried no such field,
	// so a 69 document is exactly what every saved World was yesterday.
	void aDocumentWrittenBeforeVariantsExistedLoadsAsNoVariants(
		smoke::Context const&)
	{
		core::World plain("Pre-variant world", 12, 8);
		plain.addRoom("Room", 0, 1, 1, 4, 2);
		plain.finishBuild();
		plain.pauseSimulation();
		auto document = YAML::Load(serialize(plain));
		require(document["version"].as<uint32_t>() == 74,
			"The current writer is not the version this check is bumping from");
		document["version"] = 69;
		auto const loaded = load(YAML::Dump(document));
		require(loaded->getNumSectors() == 1,
			"The pre-variant document did not load its Room");
		require(loaded->getSector(0)->getSurfaceVariants().empty(),
			"A pre-variant document loaded as though variants had been recorded");
	}

	// The field is behind its own schema version. A document that claims to
	// predate the feature but carries the field is a document that did not
	// come from this writer, and reading it anyway would be guessing.
	void aVariantFieldBelowTheSchemaVersionThatDefinesItIsRefused(
		smoke::Context const&)
	{
		core::World plain("Variant room world", 12, 8);
		plain.addRoom("Room", 0, 1, 1, 4, 2);
		plain.finishBuild();
		plain.pauseSimulation();
		auto document = YAML::Load(withVariants(serialize(plain), sampleEntries()));
		document["version"] = 69;
		require(refused([&] { (void)load(YAML::Dump(document)); }),
			"A schema 69 document carrying variant entries was accepted");
	}

	// The entries ride the construction record, so the rebuild every
	// structural edit performs replays them onto the Sector it just built.
	// Sector-local coordinates are what make this free: the resize moves and
	// reshapes the record's Sector and the entries still name the same
	// cells of it.
	void variantEntriesSurviveTheRebuildAStructuralEditPerforms(
		smoke::Context const&)
	{
		auto const world = authoredRoomWorld(sampleEntries());
		require(room(*world, 0)->getSurfaceVariants().size() == 3,
			"The fixture did not start with its three entries");

		auto const resized = world->applyLocationEdit(
			world->planResizeLocation(0, 1, 1, 5, 3));
		require(resized == 0, "The Room resize did not keep its Sector index");
		require(room(*world, 0)->getCellsWide() == 5
				&& room(*world, 0)->getLevelsHigh() == 3,
			"The Room did not take the resized footprint");
		require(room(*world, 0)->getSurfaceVariants() == sampleEntries(),
			"The structural rebuild lost the variant entries; a fact held only "
			"on the live Sector would have evaporated exactly here");
		require(room(*world, 0)->surfaceVariantAt(2, 1)
				== std::optional<uint32_t>{ 2 },
			"An entry no longer names the cell it was authored against");

		// And it survives a second edit, and a save of the edited World.
		(void)world->applyLocationEdit(world->planResizeLocation(0, 1, 1, 6, 3));
		require(room(*world, 0)->getSurfaceVariants() == sampleEntries(),
			"A second structural edit lost the variant entries");
		auto const reloaded = load(serialize(*world));
		require(room(*reloaded, 0)->getSurfaceVariants() == sampleEntries(),
			"The edited World did not save and reload its variant entries");
	}

	// An entry that names a cell the Sector does not have is a miscount, and
	// a miscount that drew nothing would be a silent failure where a
	// refusal is a loud one.
	void anEntryOutsideTheSectorIsRefused(smoke::Context const&)
	{
		require(refused([] { (void)authoredRoomWorld({ { 4, 0, 1 } }); }),
			"An entry one column past the Room's width was accepted");
		require(refused([] { (void)authoredRoomWorld({ { 0, 2, 1 } }); }),
			"An entry one level above the Room's height was accepted");

		// The last valid cell of each axis is accepted, so the bound is the
		// Sector's own extent and not one short of it.
		auto const edge = authoredRoomWorld({ { 3, 1, 1 } });
		require(edge->getSector(0)->surfaceVariantAt(3, 1)
				== std::optional<uint32_t>{ 1 },
			"The last cell of the Room's grid was refused its entry");
	}

	// Two entries for one cell is a disagreement the loader cannot resolve,
	// so it refuses rather than letting map order decide which variant the
	// cell wears.
	void aDuplicateEntryForOneCellIsRefused(smoke::Context const&)
	{
		require(refused([] {
			(void)authoredRoomWorld({ { 1, 0, 1 }, { 1, 0, 2 } });
		}), "Two entries for the same cell were accepted");
		// The same variant recorded on two different cells is not a
		// duplicate and stands.
		auto const shared = authoredRoomWorld({ { 1, 0, 4 }, { 2, 0, 4 } });
		require(shared->getSector(0)->getSurfaceVariants().size() == 2,
			"The same variant on two different cells was refused");
	}

	// A document that records variant 0 is refused at load (#1049).
	//
	// The authoring seam has always refused it, and the load seam took whatever
	// the file said. So a hand-authored `variant: 0` reached the Sector, the
	// per-cell query answered 0 for a cell that is the base, and the save kept
	// writing a field for a Sector visually identical to base. The renderer
	// papers over it - there is no `-V0` tile, so it draws the base - which is
	// exactly why the disagreement survived: the picture was right and the data
	// was wrong, so nothing looked broken and nothing got fixed.
	//
	// Refused, not repaired. A silent drop would read as a clean load and hide
	// the miscount, which is the same reasoning that made the bounds rule and
	// the duplicate rule refusals rather than repairs.
	void aVariantZeroEntryIsRefusedAtLoad(smoke::Context const&)
	{
		std::string reason;
		require(refusedWith([] { (void)authoredRoomWorld({ { 0, 0, 0 } }); }, &reason),
			"A Room record carrying variant 0 was accepted");
		require(reason.find("column 0") != std::string::npos
				&& reason.find("level 0") != std::string::npos,
			"The refusal does not name the cell it refused: " + reason);

		// The cell named is the entry's own, not a fixed one, so an author
		// reading the message can find the line that is wrong.
		require(refusedWith([] { (void)authoredRoomWorld({ { 3, 1, 0 } }); }, &reason),
			"An entry at the Room's last cell carrying variant 0 was accepted");
		require(reason.find("column 3") != std::string::npos
				&& reason.find("level 1") != std::string::npos,
			"The refusal does not name the cell it refused: " + reason);

		// A zero buried among legal entries refuses the whole load rather than
		// letting the good entries in and the bad one go unnoticed.
		require(refusedWith([] {
			(void)authoredRoomWorld({ { 0, 0, 1 }, { 1, 0, 0 }, { 2, 1, 7 } });
		}, &reason), "A variant 0 entry among legal entries was accepted");
		require(reason.find("column 1") != std::string::npos
				&& reason.find("level 0") != std::string::npos,
			"The refusal does not name the cell it refused: " + reason);

		// The refusal is the zero's doing and nothing else: the same list with
		// a real number in that slot loads all three entries.
		auto const legal = authoredRoomWorld({ { 0, 0, 1 }, { 1, 0, 2 }, { 2, 1, 7 } });
		require(legal->getSector(0)->getSurfaceVariants().size() == 3,
			"The legal list that differs from a refused one only by the variant "
			"number was refused as well");

		// The authoring seam still refuses it, unchanged. The two seams now
		// agree on what a legal entry is; neither was relaxed to meet the other.
		core::World world("Authoring seam fixture", 12, 8);
		world.addRoom("Room", 0, 1, 1, 4, 2);
		world.finishBuild();
		world.pauseSimulation();
		require(!world.canSetSectorSurfaceVariant(0, 0, 0,
				std::optional<uint32_t>{ 0 }, &reason),
			"The authoring seam accepted variant 0");
		require(reason.find("variant 0") != std::string::npos,
			"The authoring seam's refusal does not name variant 0: " + reason);
		require(!world.setSectorSurfaceVariant(0, 0, 0,
				std::optional<uint32_t>{ 0 }, &reason).accepted,
			"The authoring seam wrote variant 0 onto the World");
		require(!world.getSector(0)->surfaceVariantAt(0, 0),
			"The refused paint still left an entry on the live Sector");
		require(serialize(world).find("surfaceVariants") == std::string::npos,
			"A World whose variant paint was refused still saved a variant field");
	}

	// The refusal is the loader's rule about entries, not a Room-shaped accident
	// of one replay path. Every record type that can carry variants goes
	// through the same `applySurfaceVariants` on its way to the Sector it
	// created, and each is shown here refusing a zero and accepting a one - the
	// second half proving the refusal is about the number and not about the
	// fixture, the record type, or the wiring from record to Sector.
	void aVariantZeroEntryIsRefusedForEveryRecordTypeThatCarriesVariants(
		smoke::Context const&)
	{
		for (auto const& fixture : variantCarryingFixtures())
		{
			std::string reason;
			require(refusedWith([&] { (void)load(authoredOn(fixture, 0)); }, &reason),
				std::string("A ") + fixture.type
					+ " record carrying variant 0 was accepted");
			require(reason.find("column 0") != std::string::npos
					&& reason.find("level 0") != std::string::npos,
				std::string("The ") + fixture.type
					+ " refusal does not name the cell it refused: " + reason);

			require(anySectorWears(load(authoredOn(fixture, 1)), { { 0, 0, 1 } }),
				std::string("A ") + fixture.type
					+ " record's variant 1 entry did not reach the Sector its own "
					"record created, so the refusal above proves nothing about the "
					"number itself");
		}
	}

	// The binary wire carries the same entries and gets the same refusal.
	// Half a rule enforced only in the text form would be a rule the game's
	// own binary saves get past.
	void aVariantZeroEntryIsRefusedInTheBinaryFormat(smoke::Context const&)
	{
		// The unpatched document loads, so the refusal below is about the byte
		// that changed and not about a malformed file.
		auto const ok = loadBinary(binaryRecordingVariant(1));
		require(room(*ok, 0)->surfaceVariantAt(2, 1)
				== std::optional<uint32_t>{ 1 },
			"The binary fixture did not load the variant it recorded");

		std::string reason;
		require(refusedWith([] { (void)loadBinary(binaryRecordingVariantZero()); },
			&reason), "A binary document recording variant 0 was accepted");
		require(reason.find("column 2") != std::string::npos
				&& reason.find("level 1") != std::string::npos,
			"The binary refusal does not name the cell it refused: " + reason);
	}

	// No cell of any Sector loaded from a document answers variant 0. The
	// per-cell query is what the editor's resolve asks and what any future
	// tooling will ask; a zero there is a base cell reading as painted. Walked
	// over every cell of every Sector of every fixture, so the answer is about
	// the whole loaded World rather than the one cell a round-trip check
	// happens to look at.
	void noLoadedCellAnswersVariantZero(smoke::Context const&)
	{
		auto const walk = [](core::World const& world, std::string const& from)
		{
			for (uint32_t index = 0; index < world.getNumSectors(); ++index)
			{
				auto const& sector = *world.getSector(index);
				for (uint32_t level = 0; level < sector.getLevelsHigh(); ++level)
					for (uint32_t column = 0; column < sector.getCellsWide(); ++column)
						require(!sector.surfaceVariantAt(column, level)
								|| *sector.surfaceVariantAt(column, level) != 0,
								from + ": " + sector.getName()
									+ " answers variant 0 at column "
									+ std::to_string(column) + ", level "
									+ std::to_string(level));
			}
		};

		walk(*authoredRoomWorld(sampleEntries()), "a Room World carrying variants");
		walk(*loadBinary(serializeBinary(*authoredRoomWorld(sampleEntries()))),
			"the same World through the binary form");
		for (auto const& fixture : variantCarryingFixtures())
			walk(*load(authoredOn(fixture, 1)),
				std::string("a ") + fixture.type + " World carrying variant 1");
	}

	// A record that draws no tiled surface refuses variant entries. A
	// Background and a Facade are authored as flat colour and never ask the
	// atlas for a tile, so an entry on one is decoration that could never
	// show, and a field on a Door or Marker record means nothing at all.
	void aRecordThatDrawsNoSurfaceRefusesVariantEntries(smoke::Context const&)
	{
		for (auto const* type : { "background", "facade", "door", "marker" })
		{
			require(refused([&] {
				core::World fixture("Refusal fixture", 12, 8);
				fixture.addRoom("Room", 0, 1, 1, 4, 2);
				fixture.addRoom("Room 2", 0, 6, 1, 4, 2);
				fixture.finishBuild();
				fixture.pauseSimulation();
				auto document = YAML::Load(serialize(fixture));
				YAML::Node variants;
				YAML::Node item;
				item["column"] = 0;
				item["level"] = 0;
				item["variant"] = 1;
				variants.push_back(item);
				for (auto record : document["construction"])
					if (record.IsMap() && record["type"].as<std::string>() == "room")
					{
						record["type"] = type;
						record["surfaceVariants"] = variants;
						break;
					}
				(void)load(YAML::Dump(document));
			}), std::string("A ") + type + " record carrying variant entries "
				"was accepted");
		}
	}

	// A variant recorded on a transport's own creation record lands on
	// that transport's Sector - not on a landing corridor beside it, and
	// not on a neighbouring Room. A Room hands its index straight back to
	// the replay; a Lift's and a Ladder's come out of the creating call's
	// result, and a wiring that put the entries on the wrong Sector would
	// draw the variant somewhere its author never pointed at.
	void variantEntriesLandOnTheSectorTheirRecordCreated(smoke::Context const&)
	{
		core::World plain("Variant transport world", 16, 8);
		plain.addCorridor(0, 0, 12);
		plain.addCorridor(3, 0, 12);
		plain.addLadder(1, 0, 2, { 4, false, false });
		core::World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.stopOffsets = { 0, 3 };
		liftOptions.capacity = 2;
		plain.addLift(1, 0, 6, liftOptions);
		plain.finishBuild();
		plain.pauseSimulation();

		auto document = YAML::Load(serialize(plain));
		setRecordVariants(document, "ladder", { { 0, 0, 1 } });
		setRecordVariants(document, "lift", { { 0, 1, 2 } });
		auto const loaded = load(YAML::Dump(document));

		auto const ladder = sectorNamed(*loaded, "Ladder");
		auto const lift = sectorNamed(*loaded, "Lift");
		require(ladder != nullptr && lift != nullptr,
			"The transport fixture did not build both a Ladder and a Lift");
		require(ladder->getSurfaceVariants() == Entries{ { 0, 0, 1 } },
			"The Ladder did not carry the entries its own record recorded");
		require(lift->getSurfaceVariants() == Entries{ { 0, 1, 2 } },
			"The Lift did not carry the entries its own record recorded");
		for (uint32_t index = 0; index < loaded->getNumSectors(); ++index)
		{
			auto const& sector = *loaded->getSector(index);
			if (sector.getName() == "Ladder" || sector.getName() == "Lift") continue;
			require(sector.getSurfaceVariants().empty(),
				"A Sector that recorded nothing came back wearing a variant: "
					+ sector.getName());
		}
	}

	// Variants change nothing about traversal, capacity or simulation
	// state. The strongest statement available: the same World saved with
	// variants and saved without them differs by nothing but the variant
	// block itself.
	void variantsChangeNothingElseInTheDocument(smoke::Context const&)
	{
		core::World plain("Variant room world", 12, 8);
		plain.addRoom("Room", 0, 1, 1, 4, 2);
		plain.addCorridor(0, 3, 1, 4, 1);
		plain.finishBuild();
		plain.pauseSimulation();
		auto const plainYaml = serialize(plain);

		auto const varied = load(withVariants(plainYaml, sampleEntries()));
		auto const variedYaml = serialize(*varied);
		require(variedYaml.find("surfaceVariants") != std::string::npos,
			"The variant World saved no variant block to compare");
		require(withoutVariants(variedYaml) == withoutVariants(plainYaml),
			"Recording variants changed something in the World document beyond "
			"the variant block itself");
	}

	// Traversal and capacity, read off the live World rather than its text.
	// A variant is a picture; the walkable floor, the ends of each level and
	// how many Agents a Sector holds are untouched by it.
	void variantsChangeNoTraversalCapacityOrEndTypes(smoke::Context const&)
	{
		core::World plain("Variant room world", 12, 8);
		plain.addRoom("Room", 0, 1, 1, 4, 2);
		plain.addCorridor(0, 3, 1, 4, 1);
		plain.finishBuild();
		plain.pauseSimulation();

		auto const varied = load(withVariants(serialize(plain), sampleEntries()));

		require(varied->getNumSectors() == plain.getNumSectors(),
			"Recording variants changed how many Sectors the World has");
		for (uint32_t index = 0; index < plain.getNumSectors(); ++index)
		{
			auto const& before = *plain.getSector(index);
			auto const& after = *varied->getSector(index);
			require(before.getType() == after.getType()
					&& before.getCellsWide() == after.getCellsWide()
					&& before.getLevelsHigh() == after.getLevelsHigh()
					&& before.getCapacity() == after.getCapacity(),
				"Recording variants changed a Sector's type, footprint or capacity");
			for (uint32_t level = 0; level < before.getLevelsHigh(); ++level)
				for (int side : { CORE_SIDE_LEFT, CORE_SIDE_RIGHT })
					require(before.getEndType(level, side) == after.getEndType(level, side),
						"Recording variants changed a Sector's wall ends");
		}
		require(plain.getGraph()->getVertices().size()
				== varied->getGraph()->getVertices().size(),
			"Recording variants changed how many walkable vertices the World has");
		require(plain.getGraph()->getEdges().size()
				== varied->getGraph()->getEdges().size(),
			"Recording variants changed how many edges connect them");
	}
}
