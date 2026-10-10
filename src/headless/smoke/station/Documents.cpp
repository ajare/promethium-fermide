// #387: authored Station settings travel through the real World document
// seams. A saved document carries only validated settings - never a tick
// count, event history or Lua state - so a reopened World reconstructs a
// fresh runtime that reproduces the original scenario tick-for-tick.
// Failed writes keep the document dirty and the previous file intact;
// legacy documents load disabled; malformed ones are refused without
// disturbing the caller's loaded World; and a paused structural rebuild
// preserves the live station instead of silently restarting it.

#include "Documents.h"

#include "core/World.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string readFile(std::filesystem::path const& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::ostringstream text;
		text << in.rdbuf();
		return text.str();
	}

	void writeFile(std::filesystem::path const& path, std::string const& contents)
	{
		std::ofstream out(path, std::ios::binary);
		out << contents;
	}

	std::string serializeYaml(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData data;
		world.serialize(*writer, data);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::string serializeBinary(core::World const& world)
	{
		auto writer = core::BinarySerializer::toString();
		core::SerializationWorkData data;
		world.serialize(*writer, data);
		writer->serialize();
		return writer->getSerializedString();
	}

	void replaceAll(std::string& text, std::string const& from, std::string const& to)
	{
		for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
			text.replace(at, from.size(), to);
	}

	void enable(core::World& world, float intervalSeconds,
		std::vector<core::ResourceStoreSettings> stores = {},
		std::vector<core::ResourceTransferSettings> transfers = {})
	{
		world.pauseSimulation();
		world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems({true, intervalSeconds, std::move(stores), std::move(transfers)},
			false, &diagnostic),
			"Could not enable Station systems: " + diagnostic);
	}

	// Nothing but the authored settings may reach the document: no live tick
	// counts, no elapsed time, no event history and no Lua source or state.
	void requireSettingsOnly(std::string const& document)
	{
		require(document.find("stationSystems") != std::string::npos,
			"authored Station settings missing from the document");
		require(document.find("observationIntervalSeconds") != std::string::npos,
			"observation interval missing from the document");
		require(document.find("ratePerSecond") != std::string::npos,
			"authored transfer rate missing from the document");
		for (auto const* liveState : {"completedTicks", "elapsedSeconds", "healthy",
			"lifecycle", "advance", "worldTick", "currentQualified", "currentUnqualified",
			"lastRequested", "lastCompleted", "lastUnmet", "lastTickIn", "lastTickOut", "residual"})
			require(document.find(liveState) == std::string::npos,
				std::string("live station or Lua state leaked into the document: ") + liveState);
	}

	// Save through the real file seam and reopen: the document reconstructs a
	// fresh runtime that reproduces the original run tick-for-tick.
	void fileRoundTrip(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-files";
		filesystem::create_directories(directory);

		core::World original("Round Trip", 2, 2);
		enable(original, 2.0f, {{"o2", "kg", 1000.0, 800.0, 100.0, 500.0},
			{"water", "L", 500.0, 0.0, 250.0}, {"ballast", "kg", 42.0, 42.0, 0.0},
			{"tank", "kg", 100.0, 0.0, 0.0}},
			{{"pump", "o2", "tank", "kg", 60.0, 1}});
		original.resumeSimulation();
		require(original.advanceTicks(120), "original scenario run failed");
		auto const expected = original.getStationSystemsSnapshot();
		auto const expectedEvents = original.getStationSystemsEvents();
		require(expected.worldTick == 120 && expectedEvents.size() == 5
			&& expected.stores.size() == 4 && expected.transfers.size() == 1
			&& expected.transfers[0].lastCompleted == 0.0
			&& expected.transfers[0].status == core::StationTransferStatus::Rejected
			&& std::abs(expected.stores[3].currentQualified - 100.0) < 1e-3
			&& std::abs(expected.stores[0].currentQualified - 700.0) < 1e-3,
			"original scenario produced the wrong observable history");

		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("station") + suffix);
			original.saveTo(path.string());
			requireSettingsOnly(readFile(path));

			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"authored settings did not survive the file round trip");
			require(reopened->getStationSystemsSnapshot().stores.size() == 4
				&& reopened->getStationSystemsSnapshot().stores[0].currentQualified == 800.0
				&& reopened->getStationSystemsSnapshot().stores[3].currentQualified == 0.0,
				"reopened World did not reconstruct the authored stores");
			auto const fresh = reopened->getStationSystemsSnapshot();
			require(fresh.enabled && fresh.worldTick == 0 && fresh.elapsedSeconds == 0.0
				&& fresh.completedTicks == std::array<uint64_t, 4>{},
				"reopen did not reconstruct a fresh station runtime");
			reopened->resumeSimulation();
			require(reopened->advanceTicks(120), "reopened scenario run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"reopened scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == expectedEvents,
				"reopened event history is not reproducible from the document");
		}
	}

	// A refused save leaves the document dirty, the previous file unchanged,
	// no temporary behind, and the live World still running its new settings.
	void writeFailure(smoke::Context const& context)
	{
#if defined(_WIN32)
		(void)context;
		throw smoke::OptionalCapabilityUnavailable("POSIX permission semantics unavailable");
#else
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-write-failure";
		filesystem::create_directories(directory);
		auto const path = directory / "station.world.yaml";

		core::World world("Write Failure", 2, 2);
		enable(world, 2.0f);
		world.saveTo(path.string());
		require(!world.isModified(), "successful save did not clean the document");

		world.resumeSimulation();
		require(world.advanceTicks(60), "run before the failed save broke");
		world.pauseSimulation();
		require(world.configureStationSystems({true, 3.0f}, true), "confirmed settings edit failed");
		require(world.getSimulationTick() == 0, "confirmed edit did not reset the World");

		filesystem::permissions(directory,
			filesystem::perms::owner_read | filesystem::perms::owner_exec,
			filesystem::perm_options::replace);
		bool refused = false;
		try { world.saveTo(path.string()); }
		catch (std::exception const&) { refused = true; }
		filesystem::permissions(directory,
			filesystem::perms::owner_read | filesystem::perms::owner_write | filesystem::perms::owner_exec,
			filesystem::perm_options::replace);

		require(refused, "save into a read-only directory wrongly succeeded");
		require(world.isModified(), "a failed save cleared the document dirty state");
		require(world.getStationSystemsSettings().observationIntervalSeconds == 3.0f,
			"a failed save changed the live settings");
		for (auto const& entry : filesystem::directory_iterator(directory))
			require(entry.path() == path, "a failed save left a temporary file behind");

		auto reloaded = core::loadWorldDocument(path);
		require(reloaded->getStationSystemsSettings().observationIntervalSeconds == 2.0f,
			"a failed save corrupted the previously saved document");

		world.resumeSimulation();
		require(world.advanceTicks(60)
			&& world.getStationSystemsSnapshot().completedTicks[0] == 60,
			"the live run did not continue after the failed save");
#endif
	}

	// Old documents stay disabled, a new field under an old schema is refused
	// rather than silently dropped, and malformed values never disturb the
	// World the caller already has open.
	void legacyAndMalformedFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-legacy";
		filesystem::create_directories(directory);

		core::World plain("Plain", 2, 2);
		plain.pauseSimulation();
		plain.finishBuild();
		auto legacy = serializeYaml(plain);
		require(legacy.find("stationSystems") == std::string::npos,
			"default Station settings should not serialize a map");
		replaceAll(legacy, "version: 69", "version: 64");
		auto const legacyPath = directory / "legacy.world.yaml";
		writeFile(legacyPath, legacy);
		auto loaded = core::loadWorldDocument(legacyPath);
		require(!loaded->getStationSystemsSettings().enabled
			&& !loaded->getStationSystemsSnapshot().enabled,
			"a legacy document enabled Station systems");

		core::World station("Station", 2, 2);
		enable(station, 2.0f, {{"o2", "kg", 1000.0, 800.0, 100.0}, {"tank", "kg", 100.0, 0.0, 0.0}},
			{{"pump", "o2", "tank", "kg", 1.0, 0}});
		auto const withField = serializeYaml(station);
		require(withField.find("startingQualified") != std::string::npos
			&& withField.find("currentQualified") == std::string::npos,
			"authored store fields missing or live inventory leaked");
		bool refused = false;
		auto staleStores = withField;
		replaceAll(staleStores, "version: 69", "version: 65");
		auto const staleStoresPath = directory / "stale-stores.world.yaml";
		writeFile(staleStoresPath, staleStores);
		try { (void)core::loadWorldDocument(staleStoresPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-66 document carrying resource stores was silently accepted");
		auto staleTransfers = withField;
		replaceAll(staleTransfers, "version: 69", "version: 66");
		auto const staleTransfersPath = directory / "stale-transfers.world.yaml";
		writeFile(staleTransfersPath, staleTransfers);
		refused = false;
		try { (void)core::loadWorldDocument(staleTransfersPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-67 document carrying resource transfers was silently accepted");
		auto stale = withField;
		replaceAll(stale, "version: 69", "version: 64");
		auto const stalePath = directory / "stale.world.yaml";
		writeFile(stalePath, stale);
		refused = false;
		try { (void)core::loadWorldDocument(stalePath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-65 document carrying Station data was silently accepted");

		core::World loadedWorld("Loaded", 2, 2);
		enable(loadedWorld, 2.0f);
		loadedWorld.resumeSimulation();
		require(loadedWorld.advanceTicks(30), "loaded World run failed");
		loadedWorld.pauseSimulation();
		auto const before = loadedWorld.getStationSystemsSnapshot();

		auto malformed = withField;
		replaceAll(malformed, "observationIntervalSeconds: 2", "observationIntervalSeconds: -1");
		auto const malformedPath = directory / "malformed.world.yaml";
		writeFile(malformedPath, malformed);
		refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a malformed Station interval was accepted from a file");
		require(loadedWorld.getStationSystemsSnapshot() == before,
			"a malformed file disturbed the loaded World");

		auto malformedStore = withField;
		replaceAll(malformedStore, "capacity: 1000", "capacity: -1");
		auto const malformedStorePath = directory / "malformed-store.world.yaml";
		writeFile(malformedStorePath, malformedStore);
		refused = false;
		try { (void)core::loadWorldDocument(malformedStorePath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a malformed store capacity was accepted from a file");
		require(loadedWorld.getStationSystemsSnapshot() == before,
			"a malformed store file disturbed the loaded World");
	}

	// A paused structural rebuild keeps the live station and its settings;
	// only a reset reconstructs the runtime from the authored conditions.
	void structuralReplay(smoke::Context const& context)
	{
		(void)context;
		core::World world("Replay", 10, 2);
		world.pauseSimulation();
		world.addRoom("Room A", 0, 0, 0, 4, 1);
		world.finishBuild();
		enable(world, 1.0f);
		world.resumeSimulation();
		require(world.advanceTicks(30), "initial run failed");
		auto const live = world.getStationSystemsSnapshot();
		require(live.completedTicks[3] == 30, "station did not run before the edit");

		world.pauseSimulation();
		world.addRoom("Room B", 0, 0, 5, 4, 1);
		require(world.rebuildTraversalTopology(), "structural rebuild refused");
		require(world.getStationSystemsSnapshot() == live,
			"structural replay reset or changed the live station");
		require(world.getStationSystemsSettings().enabled,
			"structural replay lost the authored settings");

		world.resumeSimulation();
		require(world.advanceTicks(30), "run after the rebuild failed");
		require(world.getStationSystemsSnapshot().completedTicks[0] == 60,
			"station did not continue from its preserved state");

		world.pauseSimulation();
		world.resetSimulation();
		require(world.getSimulationTick() == 0
			&& world.getStationSystemsSnapshot().completedTicks[0] == 0,
			"reset did not reconstruct the station runtime");
		require(world.getStationSystemsSettings().enabled,
			"reset lost the authored settings");
	}
	// #391: declared compositions and conversion rules travel through the
	// real file seams as authored settings only; a reopened World replays the
	// compound scenario identically, and pre-68 documents carrying the new
	// fields are refused rather than silently dropped.
	void compoundFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-compounds";
		filesystem::create_directories(directory);
		auto const food = core::ResourceCompositionSettings{0.8, 0.05, 0.0, 0.01, 0.02, 0.02, 17000.0};
		auto const biomass = core::ResourceCompositionSettings{0.0, 0.04, 0.0, 0.005, 0.01, 0.005, 18000.0};
		auto const waste = core::ResourceCompositionSettings{0.8, 0.08, 0.0, 0.02, 0.04, 0.04, 8000.0};
		core::World original("Compound Round Trip", 2, 2);
		enable(original, 2.0f,
			{{"food", "kg", 100.0, 50.0, 0.0, 0.0, food},
				{"biomass", "kg", 10.0, 1.0, 0.0, 0.0, biomass},
				{"waste", "kg", 20.0, 0.0, 0.0, 0.0, waste}},
			{});
		auto settings = original.getStationSystemsSettings();
		settings.conversions.push_back({
			"harvest",
			{{"food", 1.0}},
			{{"biomass", 0.08}, {"waste", 0.4}},
			0});
		require(original.configureStationSystems(settings, true), "Compound settings edit failed");
		original.resumeSimulation();
		require(original.advanceTicks(60), "Compound original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.conversions.size() == 1
			&& expected.constituentBalances.size() == 8,
			"Compound scenario produced the wrong observable state");
		for (auto const& entry : expected.constituentBalances)
			require(std::abs(entry.residual)
				<= expected.balanceTolerance * std::max(1.0, std::abs(entry.starting)),
				"Compound replay baseline exceeded constituent tolerance");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("compound") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("composition") != std::string::npos
				&& document.find("moisture") != std::string::npos
				&& document.find("nutrientNitrogen") != std::string::npos
				&& document.find("conversions") != std::string::npos
				&& document.find("yieldPerSecond") != std::string::npos,
				"authored compound fields missing from the document");
			for (auto const* liveState : {"lastScale", "constituentLoss", "startingConstituents",
				"currentQualified", "residual", "completedTicks", "elapsedSeconds"})
				require(document.find(liveState) == std::string::npos,
					std::string("live compound state leaked into the document: ") + liveState);
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"compound settings did not survive the file round trip");
			reopened->resumeSimulation();
			require(reopened->advanceTicks(60), "Compound reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"compound scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"compound event history is not reproducible from the document");
		}
		// A pre-68 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 69", "version: 67");
			auto const stalePath = directory / "stale-compounds.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-68 document carrying compositions or conversions was silently accepted");
		}
		// A malformed composition in a file is rejected without creating
		// material out of the reader's tolerance.
		{
			auto malformed = withField;
			replaceAll(malformed, "moisture: 0.8", "moisture: 1.5");
			auto const malformedPath = directory / "malformed-composition.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a malformed moisture fraction was accepted from a file");
		}
	}
	// #393: authored solar arrays and External equipment load contracts
	// travel through the real file seams as authored settings only; a
	// reopened World replays the eclipse-supply scenario identically, and
	// pre-69 documents carrying the new fields are refused rather than
	// silently dropped.
	void powerFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-power";
		filesystem::create_directories(directory);
		core::ProfileSettings illumination;
		illumination.periodSeconds = 120.0;
		illumination.segments = {{0.0, 1.0}, {60.0, 0.0}};
		core::SolarArraySettings solar;
		solar.id = "orbital";
		solar.capacityKW = 10.0;
		solar.illumination = illumination;
		core::EquipmentLoadSettings heavy;
		heavy.id = "heavy";
		heavy.electricalDemandKW = 8.0;
		heavy.heatOutputKW = 3.0;
		heavy.thermalClass = "warm";
		heavy.coolingDisposition = core::StationCoolingDisposition::Exported;
		heavy.criticalFloorKW = 4.0;
		heavy.priority = 5;
		heavy.maxCurtailment = 0.25;
		heavy.materialExchanges.push_back({"water", 6.0, true});
		core::EquipmentLoadSettings cold;
		cold.id = "cold";
		cold.electricalDemandKW = 2.0;
		cold.thermalClass = "low";
		cold.coolingDisposition = core::StationCoolingDisposition::CoolingService;
		core::World original("Power Round Trip", 2, 2);
		enable(original, 2.0f, {{"water", "kg", 100.0, 50.0, 0.0}});
		auto settings = original.getStationSystemsSettings();
		settings.solarArrays.push_back(solar);
		settings.equipmentLoads.push_back(heavy);
		settings.equipmentLoads.push_back(cold);
		require(original.configureStationSystems(settings, true), "Power settings edit failed");
		original.resumeSimulation();
		// 61 simulated seconds: the eclipse leg, where nothing generates.
		require(original.advanceTicks(3660), "Power original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.power.sources.size() == 1
			&& std::abs(expected.power.availableGenerationKW) < 1e-9
			&& expected.power.loads.size() == 2
			&& expected.power.loads[0].id == "heavy"
			&& expected.power.loads[0].status == core::StationTransferStatus::Rejected
			&& std::abs(expected.power.loads[0].unmetCriticalKW - 4.0) < 1e-9
			&& expected.power.loads[1].id == "cold"
			&& !expected.power.loads[1].admitted,
			"Power scenario produced the wrong observable state");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("power") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("solarArrays") != std::string::npos
				&& document.find("capacityKW") != std::string::npos
				&& document.find("equipmentLoads") != std::string::npos
				&& document.find("criticalFloorKW") != std::string::npos
				&& document.find("coolingDisposition") != std::string::npos
				&& document.find("materialExchanges") != std::string::npos,
				"authored power fields missing from the document");
			for (auto const* liveState : {"suppliedKW", "unmetCriticalKW", "deliveredFraction",
				"heatExportedKW", "unmetCoolingKW", "lastIllumination"})
				require(document.find(liveState) == std::string::npos,
					std::string("live power state leaked into the document: ") + liveState);
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"power settings did not survive the file round trip");
			reopened->resumeSimulation();
			require(reopened->advanceTicks(3660), "Power reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"power scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"power event history is not reproducible from the document");
		}
		// A pre-69 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 69", "version: 68");
			auto const stalePath = directory / "stale-power.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-69 document carrying solar arrays or equipment loads was silently accepted");
		}
		// Malformed power values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "capacityKW: 10", "capacityKW: -5");
			auto const malformedPath = directory / "malformed-capacity.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a malformed solar capacity was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "coolingDisposition: coolingService", "coolingDisposition: freezer");
			auto const malformedPath = directory / "malformed-disposition.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "an unknown cooling disposition was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "criticalFloorKW: 4", "criticalFloorKW: 9");
			auto const malformedPath = directory / "malformed-floor.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a critical floor above the declared demand was accepted from a file");
		}
	}
}

void registerStationDocumentChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"fileRoundTrip", fileRoundTrip});
	checks.push_back({"writeFailure", writeFailure});
	checks.push_back({"legacyFiles", legacyAndMalformedFiles});
	checks.push_back({"structuralReplay", structuralReplay});
	checks.push_back({"compoundFiles", compoundFiles});
	checks.push_back({"powerFiles", powerFiles});
}
