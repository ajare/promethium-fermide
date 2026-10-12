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
		replaceAll(legacy, "version: 85", "version: 64");
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
		replaceAll(staleStores, "version: 85", "version: 65");
		auto const staleStoresPath = directory / "stale-stores.world.yaml";
		writeFile(staleStoresPath, staleStores);
		try { (void)core::loadWorldDocument(staleStoresPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-66 document carrying resource stores was silently accepted");
		auto staleTransfers = withField;
		replaceAll(staleTransfers, "version: 85", "version: 66");
		auto const staleTransfersPath = directory / "stale-transfers.world.yaml";
		writeFile(staleTransfersPath, staleTransfers);
		refused = false;
		try { (void)core::loadWorldDocument(staleTransfersPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-67 document carrying resource transfers was silently accepted");
		auto stale = withField;
		replaceAll(stale, "version: 85", "version: 64");
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
			replaceAll(stale, "version: 85", "version: 67");
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
	// #394: authored battery contracts travel through the real file seams
	// as authored settings only; stored energy and loss ledgers are runtime
	// state reconstructed on load, a reopened World replays the eclipse
	// battery scenario identically, and pre-70 documents carrying the new
	// fields are refused rather than silently dropped.
	void batteryFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-batteries";
		filesystem::create_directories(directory);
		core::ProfileSettings illumination;
		illumination.periodSeconds = 120.0;
		illumination.segments = {{0.0, 1.0}, {60.0, 0.0}};
		core::SolarArraySettings solar;
		solar.id = "orbital";
		solar.capacityKW = 10.0;
		solar.illumination = illumination;
		core::EquipmentLoadSettings stationLoad;
		stationLoad.id = "station";
		stationLoad.electricalDemandKW = 6.0;
		stationLoad.thermalClass = "moderate";
		stationLoad.coolingDisposition = core::StationCoolingDisposition::Exported;
		core::BatterySettings bank;
		bank.id = "bank";
		bank.nameplateKWh = 10.0;
		bank.usableKWh = 8.0;
		bank.startingStoredKWh = 8.0;
		bank.maxChargeKW = 2.0;
		bank.maxDischargeKW = 4.0;
		core::World original("Battery Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 2.0f;
		settings.solarArrays.push_back(solar);
		settings.equipmentLoads.push_back(stationLoad);
		settings.batteries.push_back(bank);
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Battery settings edit failed: " + diagnostic);
		original.resumeSimulation();
		// One second into the first eclipse: the battery is mid-ride. It
		// charged during the preceding minute of sun, so stored is above
		// its starting value but well inside the nameplate, and the
		// eclipse leg shows live discharge (#547: energy is kW x hours).
		require(original.advanceTicks(3660), "Battery original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.power.batteries.size() == 1
			&& expected.power.batteries[0].storedKWh > 8.0
			&& expected.power.batteries[0].storedKWh < 10.0
			&& expected.power.batteryDischargeKW > 0.0,
			"Battery scenario produced the wrong observable state");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("battery") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("batteries") != std::string::npos
				&& document.find("nameplateKWh") != std::string::npos
				&& document.find("usableKWh") != std::string::npos
				&& document.find("startingStoredKWh") != std::string::npos
				&& document.find("maxChargeKW") != std::string::npos
				&& document.find("maxDischargeKW") != std::string::npos
				&& document.find("chargeEfficiency") != std::string::npos
				&& document.find("dischargeEfficiency") != std::string::npos,
				"authored battery fields missing from the document");
			for (auto const* liveState : {"storedKWh", "stateOfCharge", "lastChargeKW",
				"lastDischargeKW", "chargeInputKWh", "residualKWh", "atReserve", "saturated"})
				require(document.find(liveState) == std::string::npos,
					std::string("live battery state leaked into the document: ") + liveState);
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"battery settings did not survive the file round trip");
			require(reopened->getStationSystemsSnapshot().power.batteries.size() == 1
				&& reopened->getStationSystemsSnapshot().power.batteries[0].storedKWh == 8.0,
				"reopened World did not reconstruct stored energy from the authored starting value");
			reopened->resumeSimulation();
			require(reopened->advanceTicks(3660), "Battery reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"battery scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"battery event history is not reproducible from the document");
		}
		// A pre-70 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 85", "version: 69");
			auto const stalePath = directory / "stale-batteries.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-70 document carrying batteries was silently accepted");
		}
		// Malformed battery values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "usableKWh: 8", "usableKWh: 15");
			auto const malformedPath = directory / "malformed-usable.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a usable energy above the nameplate was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "chargeEfficiency: 1", "chargeEfficiency: 0");
			auto const malformedPath = directory / "malformed-efficiency.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a zero charge efficiency was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "startingStoredKWh: 8", "startingStoredKWh: 12");
			auto const malformedPath = directory / "malformed-starting.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a starting stored energy above the nameplate was accepted from a file");
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
			replaceAll(stale, "version: 85", "version: 68");
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
	// #392: authored resupply delivery schedules travel through the real
	// file seams as authored settings only; fired occurrences, accepted and
	// rejected quantities are runtime state reconstructed on load, a
	// reopened World replays the delivery scenario identically, and
	// pre-71 documents carrying the new fields are refused rather than
	// silently dropped.
	void deliveryFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-deliveries";
		filesystem::create_directories(directory);
		core::ResourceStoreSettings tank;
		tank.id = "tank";
		tank.unit = "kg";
		tank.capacity = 60.0;
		tank.startingQualified = 10.0;
		core::ResourceDeliverySettings pulse;
		pulse.id = "pulse";
		pulse.storeId = "tank";
		pulse.unit = "kg";
		pulse.quantity = 12.0;
		pulse.firstOccurrenceSeconds = 1.0;
		pulse.recurrenceSeconds = 2.0;
		pulse.arrivesQualified = true;
		pulse.priority = 3;
		core::World original("Delivery Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 2.0f;
		settings.stores.push_back(tank);
		settings.deliveries.push_back(pulse);
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Delivery settings edit failed: " + diagnostic);
		original.resumeSimulation();
		// Ten simulated seconds: occurrences at 1, 3, 5, 7 and 9 seconds.
		// The 60 kg destination fills partway, so later arrivals are
		// refused and the refusal is what the reopened World must reproduce.
		require(original.advanceTicks(600), "Delivery original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		// Occurrences at 1, 3, 5, 7 and 9 seconds bring 12 kg each into a
		// 60 kg store that started with 10: the fourth fills it and the
		// fifth is refused whole, so both outcomes are on the record.
		require(expected.deliveries.size() == 1
			&& expected.deliveries[0].occurrencesCompleted == 5
			&& std::abs(expected.deliveries[0].acceptedTotal - 50.0) < 1e-6
			&& std::abs(expected.deliveries[0].rejectedTotal - 10.0) < 1e-6
			&& expected.stores[0].currentQualified == 60.0,
			"Delivery scenario produced the wrong observable state");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("deliveries") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("deliveries") != std::string::npos
				&& document.find("firstOccurrenceSeconds") != std::string::npos
				&& document.find("recurrenceSeconds") != std::string::npos
				&& document.find("arrivesQualified") != std::string::npos
				&& document.find("quantity") != std::string::npos,
				"authored delivery fields missing from the document");
			for (auto const* liveState : {"occurrencesCompleted", "acceptedTotal",
				"rejectedTotal", "lastAccepted", "lastRejected", "nextOccurrenceSeconds",
				"exhausted"})
				require(document.find(liveState) == std::string::npos,
					std::string("live delivery state leaked into the document: ") + liveState);
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"delivery settings did not survive the file round trip");
			auto const fresh = reopened->getStationSystemsSnapshot();
			require(fresh.deliveries.size() == 1
				&& fresh.deliveries[0].occurrencesCompleted == 0
				&& fresh.stores[0].currentQualified == 10.0
				&& !fresh.deliveries[0].exhausted,
				"reopened World did not restart the authored delivery schedule");
			reopened->resumeSimulation();
			require(reopened->advanceTicks(600), "Delivery reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"delivery scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"delivery event history is not reproducible from the document");
		}
		// A pre-71 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 85", "version: 70");
			auto const stalePath = directory / "stale-deliveries.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-71 document carrying deliveries was silently accepted");
		}
		// Malformed delivery values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "quantity: 12", "quantity: 0");
			auto const malformedPath = directory / "malformed-quantity.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a zero delivery quantity was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "storeId: tank", "storeId: nowhere");
			auto const malformedPath = directory / "unknown-delivery-target.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a delivery targeting an unknown store was accepted from a file");
		}
	}
	// #403: authored terminal-waste ejection schedules travel through the
	// real file seams as authored settings only; fired occurrences, the
	// ejected/unmet split and the external-loss bookings are runtime state
	// reconstructed on load, a reopened World replays the ejection
	// scenario identically, and pre-82 documents carrying the new fields
	// are refused rather than silently dropped.
	void wasteEjectionFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-waste-ejections";
		filesystem::create_directories(directory);
		core::ResourceStoreSettings dump;
		dump.id = "dump";
		dump.unit = "kg";
		dump.capacity = 60.0;
		dump.startingQualified = 12.0;
		dump.startingUnqualified = 5.0;
		core::WasteEjectionSettings haul;
		haul.id = "haul";
		haul.storeId = "dump";
		haul.unit = "kg";
		haul.quantity = 5.0;
		haul.firstOccurrenceSeconds = 1.0;
		haul.recurrenceSeconds = 2.0;
		haul.priority = 2;
		core::World original("Ejection Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 2.0f;
		settings.stores.push_back(dump);
		settings.wasteEjections.push_back(haul);
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Waste ejection settings edit failed: " + diagnostic);
		original.resumeSimulation();
		// Ten simulated seconds: occurrences at 1, 3, 5, 7 and 9 seconds.
		// The store starts with 12 kg disposal-ready, so the third
		// occurrence only partly fills its quota and the last two find
		// nothing eligible - held waste stays behind through all of it.
		require(original.advanceTicks(600), "Ejection original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.wasteEjections.size() == 1
			&& expected.wasteEjections[0].occurrencesCompleted == 5
			&& std::abs(expected.wasteEjections[0].ejectedTotal - 12.0) < 1e-6
			&& std::abs(expected.wasteEjections[0].unmetTotal - 13.0) < 1e-6
			&& expected.stores[0].currentQualified == 0.0
			&& expected.stores[0].currentUnqualified == 5.0,
			"Ejection scenario produced the wrong observable state");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("waste-ejections") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("wasteEjections") != std::string::npos
				&& document.find("firstOccurrenceSeconds") != std::string::npos
				&& document.find("recurrenceSeconds") != std::string::npos
				&& document.find("quantity") != std::string::npos,
				"authored waste ejection fields missing from the document");
			for (auto const* liveState : {"occurrencesCompleted", "ejectedTotal",
				"unmetTotal", "lastEjected", "lastUnmet", "nextOccurrenceSeconds",
				"exhausted"})
				require(document.find(liveState) == std::string::npos,
					std::string("live ejection state leaked into the document: ") + liveState);
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"waste ejection settings did not survive the file round trip");
			auto const fresh = reopened->getStationSystemsSnapshot();
			require(fresh.wasteEjections.size() == 1
				&& fresh.wasteEjections[0].occurrencesCompleted == 0
				&& fresh.stores[0].currentQualified == 12.0
				&& fresh.stores[0].currentUnqualified == 5.0
				&& !fresh.wasteEjections[0].exhausted,
				"reopened World did not restart the authored ejection schedule");
			reopened->resumeSimulation();
			require(reopened->advanceTicks(600), "Ejection reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"ejection scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"ejection event history is not reproducible from the document");
		}
		// A pre-82 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 85", "version: 81");
			auto const stalePath = directory / "stale-waste-ejections.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-82 document carrying waste ejections was silently accepted");
		}
		// Malformed ejection values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "quantity: 5", "quantity: 0");
			auto const malformedPath = directory / "malformed-ejection-quantity.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a zero ejection quantity was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "storeId: dump", "storeId: nowhere");
			auto const malformedPath = directory / "unknown-ejection-target.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "an ejection targeting an unknown store was accepted from a file");
		}
	}
	// #395: authored thermal reservoirs, radiators and load heat-sink
	// routing travel through the real file seams as authored settings
	// only; a reopened World replays the thermal scenario identically,
	// and pre-72 documents carrying the new fields are refused rather
	// than silently dropped.
	void thermalFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-thermal";
		filesystem::create_directories(directory);
		core::ThermalReservoirSettings cabin;
		cabin.id = "cabin";
		cabin.thermalClass = "moderate";
		cabin.heatCapacityKJPerK = 100.0;
		cabin.startingTemperatureC = 20.0;
		cabin.minTemperatureC = 10.0;
		cabin.maxTemperatureC = 60.0;
		cabin.absorbedHeatKW = 0.5;
		core::RadiatorSettings panel;
		panel.id = "panel";
		panel.reservoirId = "cabin";
		panel.areaM2 = 5000.0;
		panel.emissivity = 0.9;
		panel.viewFactor = 1.0;
		panel.sinkTemperatureC = -270.0;
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 10.0;
		core::EquipmentLoadSettings shop;
		shop.id = "shop";
		shop.electricalDemandKW = 5.0;
		shop.thermalClass = "moderate";
		shop.heatOutputKW = 2.0;
		shop.heatSinkId = "cabin";
		core::World original("Thermal Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 2.0f;
		settings.thermalReservoirs.push_back(cabin);
		settings.radiators.push_back(panel);
		settings.solarArrays.push_back(field);
		settings.equipmentLoads.push_back(shop);
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Thermal settings edit failed: " + diagnostic);
		original.resumeSimulation();
		require(original.advanceTicks(3600), "Thermal original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.thermal.reservoirs.size() == 1
			&& expected.thermal.reservoirs[0].temperatureC > 20.0
			&& !expected.thermal.reservoirs[0].aboveLimit
			&& std::abs(expected.thermal.reservoirs[0].residualKJ) < 1e-6
			&& expected.thermal.radiators[0].lastRejectedKW > 0.0
			&& std::abs(expected.thermal.reservoirs[0].temperatureC - 20.0) < 40.0,
			"Thermal scenario produced the wrong observable state");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("thermal") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("thermalReservoirs") != std::string::npos
				&& document.find("heatCapacityKJPerK") != std::string::npos
				&& document.find("startingTemperatureC") != std::string::npos
				&& document.find("minTemperatureC") != std::string::npos
				&& document.find("maxTemperatureC") != std::string::npos
				&& document.find("absorbedHeatKW") != std::string::npos
				&& document.find("radiators") != std::string::npos
				&& document.find("areaM2") != std::string::npos
				&& document.find("emissivity") != std::string::npos
				&& document.find("viewFactor") != std::string::npos
				&& document.find("sinkTemperatureC") != std::string::npos
				&& document.find("heatSinkId") != std::string::npos,
				"authored thermal fields missing from the document");
			for (auto const* liveState : {"temperatureC", "aboveLimit", "belowLimit",
				"residualKJ", "rejectedEnergyKJ", "routedEnergyKJ", "absorbedEnergyKJ",
				"lastRejectedKW", "currentEnergyKJ"})
				require(document.find(liveState) == std::string::npos,
					std::string("live thermal state leaked into the document: ") + liveState);
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"thermal settings did not survive the file round trip");
			require(reopened->getStationSystemsSnapshot().thermal.reservoirs.size() == 1
				&& reopened->getStationSystemsSnapshot().thermal.reservoirs[0].temperatureC == 20.0
				&& reopened->getStationSystemsSnapshot().thermal.reservoirs[0].absorbedEnergyKJ == 0.0,
				"reopened World did not reconstruct the authored thermal starting state");
			reopened->resumeSimulation();
			require(reopened->advanceTicks(3600), "Thermal reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"thermal scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"thermal event history is not reproducible from the document");
		}
		// A pre-72 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 85", "version: 71");
			auto const stalePath = directory / "stale-thermal.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-72 document carrying thermal reservoirs was silently accepted");
		}
		// Malformed thermal values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "heatCapacityKJPerK: 100", "heatCapacityKJPerK: 0");
			auto const malformedPath = directory / "malformed-capacity.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a zero thermal capacity was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "emissivity: 0.9", "emissivity: 1.5");
			auto const malformedPath = directory / "malformed-emissivity.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "an emissivity above one was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "sinkTemperatureC: -270", "sinkTemperatureC: -300");
			auto const malformedPath = directory / "malformed-sink.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a sub-absolute-zero sink temperature was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "heatSinkId: cabin", "heatSinkId: ghost");
			auto const malformedPath = directory / "malformed-heatsink.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a load heat sink targeting an unknown reservoir was accepted from a file");
		}
	}
	// #398: authored thermal buffers travel through the real file seams as
	// authored contracts only; a reopened World replays the buffer scenario
	// identically, and pre-81 documents carrying `thermalBuffers` are refused
	// rather than silently dropped. Stored energy, fill, buffer temperature and
	// the charge/discharge ledger stay runtime-only.
	void bufferFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-buffer";
		filesystem::create_directories(directory);
		core::ThermalReservoirSettings host;
		host.id = "host";
		host.thermalClass = "moderate";
		host.heatCapacityKJPerK = 100.0;
		host.startingTemperatureC = 50.0;
		host.minTemperatureC = 0.0;
		host.maxTemperatureC = 200.0;
		host.absorbedHeatKW = 10.0;
		core::ThermalBufferSettings buf;
		buf.id = "buf";
		buf.reservoirId = "host";
		buf.accessMode = "passive";
		buf.capacityKJ = 1000.0;
		buf.startingEnergyKJ = 100.0;
		buf.minUsableTemperatureC = 0.0;
		buf.maxUsableTemperatureC = 40.0;
		buf.maxChargeKW = 20.0;
		buf.maxDischargeKW = 20.0;
		buf.transferConductanceKWPerK = 1e6;
		core::World original("Buffer Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 2.0f;
		settings.thermalReservoirs.push_back(host);
		settings.thermalBuffers.push_back(buf);
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Buffer settings edit failed: " + diagnostic);
		original.resumeSimulation();
		require(original.advanceTicks(3600), "Buffer original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.thermalBuffers.size() == 1
			&& expected.thermalBuffers[0].chargedEnergyKJ > 0.0
			&& expected.thermalBuffers[0].storedEnergyKJ > 100.0
			&& std::abs(expected.thermalBuffers[0].residualKJ) < 1e-6,
			"Buffer scenario produced the wrong observable state");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("buffer") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("thermalBuffers") != std::string::npos
				&& document.find("capacityKJ") != std::string::npos
				&& document.find("startingEnergyKJ") != std::string::npos
				&& document.find("minUsableTemperatureC") != std::string::npos
				&& document.find("maxUsableTemperatureC") != std::string::npos
				&& document.find("maxChargeKW") != std::string::npos
				&& document.find("maxDischargeKW") != std::string::npos
				&& document.find("transferConductanceKWPerK") != std::string::npos
				&& document.find("accessMode") != std::string::npos,
				"authored thermal buffer fields missing from the document");
			for (auto const* liveState : {"storedEnergyKJ", "availableCapacityKJ",
				"fillFraction", "bufferTemperatureC", "lastChargeKW", "lastDischargeKW",
				"chargedEnergyKJ", "dischargedEnergyKJ", "residualKJ", "accessible",
				"lastLimiter"})
				require(document.find(liveState) == std::string::npos,
					std::string("live thermal buffer state leaked into the document: ") + liveState);
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"thermal buffer settings did not survive the file round trip");
			require(reopened->getStationSystemsSnapshot().thermalBuffers.size() == 1
				&& reopened->getStationSystemsSnapshot().thermalBuffers[0].storedEnergyKJ == 100.0
				&& reopened->getStationSystemsSnapshot().thermalBuffers[0].chargedEnergyKJ == 0.0
				&& reopened->getStationSystemsSnapshot().thermalBuffers[0].dischargedEnergyKJ == 0.0,
				"reopened World did not reconstruct the authored buffer starting energy");
			reopened->resumeSimulation();
			require(reopened->advanceTicks(3600), "Buffer reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"thermal buffer scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"thermal buffer event history is not reproducible from the document");
		}
		// A pre-81 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 85", "version: 80");
			auto const stalePath = directory / "stale-buffer.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-81 document carrying thermal buffers was silently accepted");
		}
		// Malformed buffer values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "capacityKJ: 1000", "capacityKJ: 0");
			auto const malformedPath = directory / "malformed-buffer-capacity.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a zero thermal buffer capacity was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "startingEnergyKJ: 100", "startingEnergyKJ: 5000");
			auto const malformedPath = directory / "malformed-buffer-starting.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a thermal buffer starting energy above capacity was accepted from a file");
		}
	}
	// #396: authored coolant loops travel through the real file seams as
	// authored contracts only; a reopened World replays the coolant
	// scenario identically, and pre-73 documents carrying `coolantLoops`
	// are refused rather than silently dropped. Coolant inventory,
	// circulation and the loop energy ledger stay runtime-only.
	void coolantFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-coolant";
		filesystem::create_directories(directory);
		core::ThermalReservoirSettings bus;
		bus.id = "bus";
		bus.thermalClass = "moderate";
		bus.heatCapacityKJPerK = 1000.0;
		bus.startingTemperatureC = 20.0;
		bus.minTemperatureC = -50.0;
		bus.maxTemperatureC = 100.0;
		core::CoolantLoopSettings normalLoop;
		normalLoop.id = "itcs";
		normalLoop.hazardClass = "normal";
		normalLoop.reservoirId = "bus";
		normalLoop.coolantCapacityKg = 100.0;
		normalLoop.startingCoolantKg = 100.0;
		normalLoop.specificHeatKJPerKgK = 4.0;
		normalLoop.startingTemperatureC = 20.0;
		normalLoop.freezeProtectionSetpointC = 5.0;
		normalLoop.pumpPowerKW = 1.2;
		normalLoop.ratedFlowKgPerS = 3.0;
		normalLoop.exchangerConductanceKWPerK = 6.0;
		normalLoop.heaterCapacityKW = 2.0;
		core::CoolantLoopSettings hazLoop;
		hazLoop.id = "htcs";
		hazLoop.hazardClass = "hazardous";
		hazLoop.reservoirId = "bus";
		hazLoop.coolantCapacityKg = 50.0;
		hazLoop.startingCoolantKg = 25.0;
		hazLoop.specificHeatKJPerKgK = 2.5;
		hazLoop.startingTemperatureC = 10.0;
		hazLoop.freezeProtectionSetpointC = 2.0;
		hazLoop.pumpPowerKW = 0.8;
		hazLoop.ratedFlowKgPerS = 1.5;
		hazLoop.exchangerConductanceKWPerK = 3.0;
		hazLoop.heaterCapacityKW = 1.0;
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 10.0;
		core::World original("Coolant Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 2.0f;
		settings.thermalReservoirs.push_back(bus);
		settings.coolantLoops.push_back(normalLoop);
		settings.coolantLoops.push_back(hazLoop);
		settings.solarArrays.push_back(field);
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Coolant settings edit failed: " + diagnostic);
		original.resumeSimulation();
		require(original.advanceTicks(3600), "Coolant original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.coolantLoops.size() == 2
			&& expected.coolantLoops[0].hazardClass == "normal"
			&& expected.coolantLoops[1].hazardClass == "hazardous"
			&& expected.coolantLoops[0].flowKgPerS > 0.0
			&& std::abs(expected.coolantLoops[0].residualKJ) < 1e-5
			&& std::abs(expected.coolantLoops[1].residualKJ) < 1e-5
			&& std::abs(expected.thermal.reservoirs[0].residualKJ) < 1e-5,
			"Coolant scenario produced the wrong observable state");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("coolant") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("coolantLoops") != std::string::npos
				&& document.find("hazardClass") != std::string::npos
				&& document.find("reservoirId") != std::string::npos
				&& document.find("coolantCapacityKg") != std::string::npos
				&& document.find("startingCoolantKg") != std::string::npos
				&& document.find("specificHeatKJPerKgK") != std::string::npos
				&& document.find("freezeProtectionSetpointC") != std::string::npos
				&& document.find("pumpPowerKW") != std::string::npos
				&& document.find("ratedFlowKgPerS") != std::string::npos
				&& document.find("exchangerConductanceKWPerK") != std::string::npos
				&& document.find("heaterCapacityKW") != std::string::npos,
				"authored coolant fields missing from the document");
			for (auto const* liveState : {"circulationFraction", "belowFreezeSetpoint",
				"pumpSuppliedKW", "heaterSuppliedKW", "exchangedEnergyKJ",
				"pumpWorkEnergyKJ", "loopTemperatureC", "fillFraction"})
				require(document.find(liveState) == std::string::npos,
					std::string("live coolant state leaked into the document: ") + liveState);
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"coolant settings did not survive the file round trip");
			require(reopened->getStationSystemsSnapshot().coolantLoops.size() == 2
				&& reopened->getStationSystemsSnapshot().coolantLoops[0].loopTemperatureC == 20.0
				&& reopened->getStationSystemsSnapshot().coolantLoops[0].exchangedEnergyKJ == 0.0
				&& reopened->getStationSystemsSnapshot().coolantLoops[1].loopTemperatureC == 10.0,
				"reopened World did not reconstruct the authored coolant starting state");
			reopened->resumeSimulation();
			require(reopened->advanceTicks(3600), "Coolant reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"coolant scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"coolant event history is not reproducible from the document");
		}
		// A pre-73 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 85", "version: 72");
			auto const stalePath = directory / "stale-coolant.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-73 document carrying coolant loops was silently accepted");
		}
		// Malformed coolant values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "coolantCapacityKg: 100", "coolantCapacityKg: 0");
			auto const malformedPath = directory / "malformed-coolant-capacity.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a zero coolant capacity was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "hazardClass: normal", "hazardClass: toxic");
			auto const malformedPath = directory / "malformed-hazard.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "an unknown coolant hazard class was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "reservoirId: bus", "reservoirId: ghost");
			auto const malformedPath = directory / "malformed-loop-host.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a coolant loop coupled to an unknown reservoir was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "startingCoolantKg: 100", "startingCoolantKg: 150");
			auto const malformedPath = directory / "malformed-overfill.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "starting coolant above the loop capacity was accepted from a file");
		}
	}
	// #397: authored chiller contracts travel through the real file
	// seams as contracts only; a reopened World replays the chilled
	// service scenario identically, and pre-74 documents carrying
	// `chillers` are refused rather than silently dropped. Cold-side
	// duty, work, condenser heat and the chiller ledgers stay runtime-only.
	void chillerFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-chillers";
		filesystem::create_directories(directory);
		core::ThermalReservoirSettings cold;
		cold.id = "cold";
		cold.thermalClass = "moderate";
		cold.heatCapacityKJPerK = 1000.0;
		cold.startingTemperatureC = 20.0;
		cold.minTemperatureC = -50.0;
		cold.maxTemperatureC = 200.0;
		core::ThermalReservoirSettings sink;
		sink.id = "sink";
		sink.thermalClass = "moderate";
		sink.heatCapacityKJPerK = 1000.0;
		sink.startingTemperatureC = 20.0;
		sink.minTemperatureC = -50.0;
		sink.maxTemperatureC = 200.0;
		core::CoolantLoopSettings chilled;
		chilled.id = "chilled";
		chilled.hazardClass = "normal";
		chilled.reservoirId = "cold";
		chilled.coolantCapacityKg = 100.0;
		chilled.startingCoolantKg = 100.0;
		chilled.specificHeatKJPerKgK = 4.0;
		chilled.startingTemperatureC = 20.0;
		chilled.freezeProtectionSetpointC = 0.0;
		chilled.pumpPowerKW = 0.5;
		chilled.ratedFlowKgPerS = 2.0;
		chilled.exchangerConductanceKWPerK = 1.0;
		chilled.heaterCapacityKW = 0.0;
		core::ChillerSettings chiller;
		chiller.id = "ics";
		chiller.thermalClass = "moderate";
		chiller.coldLoopId = "chilled";
		chiller.condenserReservoirId = "sink";
		chiller.ratedCoolingCapacityKW = 6.0;
		chiller.cop = 3.0;
		chiller.auxiliaryPowerKW = 0.5;
		chiller.chilledSetpointC = 5.0;
		chiller.maxCondensingTemperatureC = 45.0;
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 3.0;
		core::World original("Chiller Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 2.0f;
		settings.thermalReservoirs.push_back(cold);
		settings.thermalReservoirs.push_back(sink);
		settings.coolantLoops.push_back(chilled);
		settings.chillers.push_back(chiller);
		settings.solarArrays.push_back(field);
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Chiller settings edit failed: " + diagnostic);
		original.resumeSimulation();
		require(original.advanceTicks(3600), "Chiller original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.chillers.size() == 1
			&& expected.chillers[0].coolingKW > 0.0
			&& std::abs(expected.chillers[0].residualKJ) < 1e-4
			&& std::abs(expected.thermal.reservoirs[1].residualKJ) < 1e-4
			&& std::abs(expected.coolantLoops[0].residualKJ) < 1e-4,
			"Chiller scenario produced the wrong observable state");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("chillers") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("chillers") != std::string::npos
				&& document.find("thermalClass") != std::string::npos
				&& document.find("coldLoopId") != std::string::npos
				&& document.find("condenserReservoirId") != std::string::npos
				&& document.find("ratedCoolingCapacityKW") != std::string::npos
				&& document.find("cop") != std::string::npos
				&& document.find("auxiliaryPowerKW") != std::string::npos
				&& document.find("chilledSetpointC") != std::string::npos
				&& document.find("maxCondensingTemperatureC") != std::string::npos,
				"authored chiller fields missing from the document");
			for (auto const* liveState : {"coolingUnmetKW", "powerSuppliedKW",
				"condenserHeatKW", "deliveredFraction", "extractedEnergyKJ",
				"workEnergyKJ", "condenserEnergyKJ", "atChilledSetpoint",
				"sinkOverMaxCondensing"})
				require(document.find(liveState) == std::string::npos,
					std::string("live chiller state leaked into the document: ") + liveState);
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"chiller settings did not survive the file round trip");
			require(reopened->getStationSystemsSnapshot().chillers.size() == 1
				&& reopened->getStationSystemsSnapshot().chillers[0].extractedEnergyKJ == 0.0
				&& reopened->getStationSystemsSnapshot().chillers[0].condenserHeatKW == 0.0
				&& reopened->getStationSystemsSnapshot().thermal.reservoirs[1].condenserEnergyKJ == 0.0,
				"reopened World did not reconstruct the authored chiller starting state");
			reopened->resumeSimulation();
			require(reopened->advanceTicks(3600), "Chiller reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"chiller scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"chiller event history is not reproducible from the document");
		}
		// A pre-74 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 85", "version: 73");
			auto const stalePath = directory / "stale-chillers.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-74 document carrying chillers was silently accepted");
		}
		// Malformed chiller values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "cop: 3", "cop: 0");
			auto const malformedPath = directory / "malformed-chiller-cop.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a zero chiller COP was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "coldLoopId: chilled", "coldLoopId: ghost");
			auto const malformedPath = directory / "malformed-chiller-loop.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a chiller on an unknown cold loop was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "condenserReservoirId: sink", "condenserReservoirId: cold");
			auto const malformedPath = directory / "malformed-chiller-self-loop.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a chiller rejecting into its own cold-side reservoir was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "chilledSetpointC: 5", "chilledSetpointC: 50");
			auto const malformedPath = directory / "malformed-chiller-setpoint.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a chilled setpoint above the maximum condensing temperature was accepted from a file");
		}
	}
	void generatorFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-generators";
		filesystem::create_directories(directory);
		core::ResourceStoreSettings diesel;
		diesel.id = "diesel";
		diesel.unit = "kWh";
		diesel.capacity = 1000.0;
		diesel.startingQualified = 1000.0;
		diesel.protectedReserve = 0.0;
		core::ThermalReservoirSettings sink;
		sink.id = "hot";
		sink.thermalClass = "generation";
		sink.heatCapacityKJPerK = 10000.0;
		sink.startingTemperatureC = 20.0;
		sink.minTemperatureC = -50.0;
		sink.maxTemperatureC = 200.0;
		core::GeneratorSettings generator;
		generator.id = "gen";
		generator.fuelStoreId = "diesel";
		generator.thermalClass = "generation";
		generator.heatSinkId = "hot";
		generator.ratedOutputKW = 10.0;
		generator.efficiency = 0.5;
		generator.maxRampKWPerSecond = 5.0;
		generator.commandedOutputKW = 10.0;
		generator.startupPowerKW = 1.0;
		generator.startupSeconds = 6.0;
		generator.shutdownHeatKW = 1.0;
		generator.shutdownSeconds = 10.0;
		generator.maxSinkTemperatureC = 90.0;
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 20.0;
		core::World original("Generator Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 2.0f;
		settings.stores.push_back(diesel);
		settings.thermalReservoirs.push_back(sink);
		settings.generators.push_back(generator);
		settings.solarArrays.push_back(field);
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Generator settings edit failed: " + diagnostic);
		original.resumeSimulation();
		require(original.advanceTicks(3600), "Generator original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.power.generators.size() == 1
			&& expected.power.generators[0].outputKW > 0.0
			&& std::abs(expected.power.generators[0].residualKJ) < 1e-4
			&& std::abs(expected.thermal.reservoirs[0].residualKJ) < 1e-4,
			"Generator scenario produced the wrong observable state");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("generators") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("generators") != std::string::npos
				&& document.find("fuelStoreId") != std::string::npos
				&& document.find("heatSinkId") != std::string::npos
				&& document.find("ratedOutputKW") != std::string::npos
				&& document.find("efficiency") != std::string::npos
				&& document.find("maxRampKWPerSecond") != std::string::npos
				&& document.find("commandedOutputKW") != std::string::npos
				&& document.find("startupPowerKW") != std::string::npos
				&& document.find("startupSeconds") != std::string::npos
				&& document.find("shutdownHeatKW") != std::string::npos
				&& document.find("shutdownSeconds") != std::string::npos
				&& document.find("maxSinkTemperatureC") != std::string::npos,
				"authored generator fields missing from the document");
			for (auto const* liveState : {"outputKW", "startupReceivedKJ",
				"fuelEnergyKJ", "electricalKJ", "wasteHeatKJ", "decayHeatKJ",
				"blockReason", "coolingAvailable", "residualKJ", "phase"})
				require(document.find(liveState) == std::string::npos,
					std::string("live generator state leaked into the document: ") + liveState);
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"generator settings did not survive the file round trip");
			require(reopened->getStationSystemsSnapshot().power.generators.size() == 1
				&& reopened->getStationSystemsSnapshot().power.generators[0].outputKW == 0.0
				&& reopened->getStationSystemsSnapshot().power.generators[0].fuelEnergyKJ == 0.0
				&& reopened->getStationSystemsSnapshot().thermal.reservoirs[0].generatorEnergyKJ == 0.0,
				"reopened World did not reconstruct the authored generator starting state");
			reopened->resumeSimulation();
			require(reopened->advanceTicks(3600), "Generator reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"generator scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"generator event history is not reproducible from the document");
		}
		// A pre-75 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 85", "version: 74");
			auto const stalePath = directory / "stale-generators.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-75 document carrying generators was silently accepted");
		}
		// Malformed generator values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "efficiency: 0.5", "efficiency: 0");
			auto const malformedPath = directory / "malformed-generator-efficiency.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a zero generator efficiency was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "fuelStoreId: diesel", "fuelStoreId: ghost");
			auto const malformedPath = directory / "malformed-generator-fuel.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a generator on an unknown fuel store was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "commandedOutputKW: 10", "commandedOutputKW: 11");
			auto const malformedPath = directory / "malformed-generator-command.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a commanded output above the installed rating was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "maxRampKWPerSecond: 5", "maxRampKWPerSecond: 0");
			auto const malformedPath = directory / "malformed-generator-ramp.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a zero ramp limit was accepted from a file");
		}
	}

	// #400: compartment atmospheres and scheduled airlock cycles persist
	// through the document seam - YAML and binary roundtrips, the reopened
	// World reproducing the closed scenario from the authored starting
	// state, pre-76 documents carrying the fields refused, and malformed
	// compartment or cycle values rejected at load.
	void atmosphereFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-atmospheres";
		filesystem::create_directories(directory);
		core::ThermalReservoirSettings bus;
		bus.id = "bus";
		bus.thermalClass = "moderate";
		bus.heatCapacityKJPerK = 50000.0;
		bus.startingTemperatureC = 40.0;
		bus.minTemperatureC = -50.0;
		bus.maxTemperatureC = 200.0;
		core::AtmosphereSettings dock;
		dock.id = "dock";
		dock.hazardClass = "normal";
		dock.volumeM3 = 100.0;
		dock.startingOxygenKg = 20.0;
		dock.startingNitrogenKg = 80.0;
		dock.startingTemperatureC = 20.0;
		dock.thermalCapacityKJPerK = 10000.0;
		dock.reservoirId = "bus";
		dock.exchangerConductanceKWPerK = 2.0;
		dock.occupants = 2;
		dock.leakRateKgPerS = 0.01;
		dock.minPressureKPa = 1.0;
		dock.maxPressureKPa = 200.0;
		core::AtmosphereSettings plant;
		plant.id = "plant";
		plant.hazardClass = "hazardous";
		plant.volumeM3 = 50.0;
		plant.startingOxygenKg = 5.0;
		plant.startingNitrogenKg = 45.0;
		plant.startingCarbonDioxideKg = 2.0;
		plant.startingWaterVapourKg = 0.4;
		plant.startingTemperatureC = 25.0;
		plant.thermalCapacityKJPerK = 5000.0;
		plant.minPressureKPa = 1.0;
		plant.maxPressureKPa = 200.0;
		core::AirlockCycleSettings crew;
		crew.id = "crew";
		crew.atmosphereId = "dock";
		crew.gasMassKg = 1.0;
		crew.firstOccurrenceSeconds = 10.0;
		crew.recurrenceSeconds = 5.0;
		crew.recoveredFraction = 0.9;
		core::AirlockCycleSettings purge;
		purge.id = "purge";
		purge.atmosphereId = "plant";
		purge.gasMassKg = 2.0;
		purge.firstOccurrenceSeconds = 20.0;
		purge.recoveredFraction = 0.5;
		core::World original("Atmosphere Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 2.0f;
		settings.thermalReservoirs.push_back(bus);
		settings.atmospheres.push_back(dock);
		settings.atmospheres.push_back(plant);
		settings.airlockCycles.push_back(crew);
		settings.airlockCycles.push_back(purge);
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Atmosphere settings edit failed: " + diagnostic);
		original.resumeSimulation();
		require(original.advanceTicks(1800), "Atmosphere original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.atmospheres.size() == 2 && expected.airlockCycles.size() == 2,
			"Atmosphere scenario did not produce the expected snapshot shape");
		require(expected.atmospheres[0].leakedKg > 0.0
			&& expected.airlockCycles[0].occurrencesCompleted == 5,
			"Atmosphere scenario did not exercise leak and cycles before saving");
		auto const path = directory / "atmospheres.world.yaml";
		original.saveTo(path.string());
		require(readFile(path).find("atmospheres:") != std::string::npos,
			"saved document lacks atmospheres");
		require(readFile(path).find("airlockCycles:") != std::string::npos,
			"saved document lacks airlockCycles");
		auto const reopened = core::loadWorldDocument(path);
		reopened->pauseSimulation();
		require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
			"authored atmosphere settings did not survive the file round trip");
		require(reopened->getStationSystemsSnapshot().atmospheres[0].leakedKg == 0.0
			&& reopened->getStationSystemsSnapshot().atmospheres[0].temperatureC == 20.0
			&& reopened->getStationSystemsSnapshot().airlockCycles[0].occurrencesCompleted == 0,
			"reopened World did not reconstruct the authored atmosphere starting state");
		reopened->resumeSimulation();
		require(reopened->advanceTicks(1800), "Atmosphere reopened run failed");
		require(reopened->getStationSystemsSnapshot() == expected,
			"Atmosphere scenario is not reproducible from the document");
		require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
			"Atmosphere event history is not reproducible from the document");
		auto const binaryPath = directory / "atmospheres.world";
		original.saveTo(binaryPath.string());
		auto const reopenedBinary = core::loadWorldDocument(binaryPath);
		reopenedBinary->pauseSimulation();
		require(reopenedBinary->getStationSystemsSnapshot().atmospheres.size() == 2
			&& reopenedBinary->getStationSystemsSnapshot().airlockCycles.size() == 2
			&& reopenedBinary->getStationSystemsSnapshot().atmospheres[0].leakedKg == 0.0,
			"reopened binary Atmosphere document did not reconstruct the starting state");
		// A pre-76 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 85", "version: 75");
			auto const stalePath = directory / "stale-atmospheres.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-76 document carrying atmospheres was silently accepted");
		}
		// Malformed atmosphere values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "volumeM3: 100", "volumeM3: 0");
			auto const malformedPath = directory / "malformed-atmosphere-volume.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a zero compartment volume was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "hazardClass: hazardous", "hazardClass: plasma");
			auto const malformedPath = directory / "malformed-atmosphere-hazard.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "an unknown hazard class was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "atmosphereId: dock", "atmosphereId: ghost");
			auto const malformedPath = directory / "malformed-airlock-atmosphere.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "an airlock cycle on an unknown compartment was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "recoveredFraction: 0.9", "recoveredFraction: 1.5");
			auto const malformedPath = directory / "malformed-airlock-recovery.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a recovered fraction above one was accepted from a file");
		}
	}
	// #401: the authored crew contract travels through the real document
	// seams; consumption, returns and the retained ledger are runtime
	// state reconstructed from the authored starting conditions.
	void crewFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-crews";
		filesystem::create_directories(directory);
		core::ResourceCompositionSettings foodComposition;
		foodComposition.moisture = 0.8;
		foodComposition.nutrientNitrogen = 0.05;
		foodComposition.phosphorus = 0.01;
		foodComposition.potassium = 0.02;
		foodComposition.otherMinerals = 0.02;
		foodComposition.energyKJPerKgDry = 17000.0;
		core::ResourceCompositionSettings waterComposition;
		waterComposition.moisture = 0.999;
		waterComposition.otherMinerals = 0.001;
		core::ResourceCompositionSettings urineComposition;
		urineComposition.moisture = 0.95;
		urineComposition.nutrientNitrogen = 0.1;
		urineComposition.phosphorus = 0.02;
		urineComposition.potassium = 0.05;
		urineComposition.otherMinerals = 0.05;
		urineComposition.energyKJPerKgDry = 500.0;
		core::ResourceCompositionSettings wasteComposition;
		wasteComposition.moisture = 0.5;
		wasteComposition.energyKJPerKgDry = 10000.0;
		auto store = [](std::string const& id, double capacity, double starting,
			core::ResourceCompositionSettings composition)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.composition = composition;
			return settings;
		};
		core::AtmosphereSettings habitat;
		habitat.id = "habitat";
		habitat.hazardClass = "normal";
		habitat.volumeM3 = 100.0;
		habitat.startingOxygenKg = 20.0;
		habitat.startingNitrogenKg = 80.0;
		habitat.startingTemperatureC = 20.0;
		habitat.thermalCapacityKJPerK = 10000.0;
		habitat.minPressureKPa = 1.0;
		habitat.maxPressureKPa = 200.0;
		core::CrewSettings crew;
		crew.id = "crew";
		crew.atmosphereId = "habitat";
		crew.crewCount = 2;
		crew.activityProfile.periodSeconds = 120.0;
		crew.activityProfile.segments = { {0.0, 1.0}, {60.0, 0.4} };
		crew.foodStoreId = "food";
		crew.waterStoreId = "water";
		crew.urineStoreId = "urine";
		crew.organicWasteStoreId = "organicWaste";
		crew.foodPerCrewPerS = 0.001;
		crew.waterPerCrewPerS = 0.002;
		crew.oxygenPerCrewKgPerS = 0.001;
		crew.co2PerCrewKgPerS = 0.0011;
		crew.moisturePerCrewKgPerS = 0.0005;
		crew.urinePerCrewPerS = 0.0015;
		crew.organicWastePerCrewPerS = 0.0002;
		crew.metabolicHeatPerCrewKW = 0.1;
		core::World original("Crew Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 2.0f;
		settings.stores.push_back(store("food", 100.0, 50.0, foodComposition));
		settings.stores.push_back(store("water", 100.0, 50.0, waterComposition));
		settings.stores.push_back(store("urine", 100.0, 0.0, urineComposition));
		settings.stores.push_back(store("organicWaste", 100.0, 0.0, wasteComposition));
		settings.atmospheres.push_back(habitat);
		settings.crews.push_back(crew);
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Crew settings edit failed: " + diagnostic);
		original.resumeSimulation();
		require(original.advanceTicks(1800), "Crew original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.crews.size() == 1
			&& expected.crews[0].foodConsumed > 0.0
			&& expected.crews[0].retainedKg > 0.0
			&& expected.crews[0].urineProduced > 0.0,
			"Crew scenario did not exercise consumption and returns before saving");
		auto const path = directory / "crews.world.yaml";
		original.saveTo(path.string());
		require(readFile(path).find("crews:") != std::string::npos,
			"saved document lacks crews");
		require(readFile(path).find("metabolicHeatPerCrewKW") != std::string::npos,
			"saved document lacks the crew metabolic contract");
		auto const reopened = core::loadWorldDocument(path);
		reopened->pauseSimulation();
		require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
			"authored crew settings did not survive the file round trip");
		require(reopened->getStationSystemsSnapshot().crews[0].foodConsumed == 0.0
			&& reopened->getStationSystemsSnapshot().crews[0].retainedKg == 0.0
			&& reopened->getStationSystemsSnapshot().stores[2].currentQualified == 0.0,
			"reopened World did not reconstruct the crew starting state");
		reopened->resumeSimulation();
		require(reopened->advanceTicks(1800), "Crew reopened run failed");
		require(reopened->getStationSystemsSnapshot() == expected,
			"Crew scenario is not reproducible from the document");
		require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
			"Crew event history is not reproducible from the document");
		auto const binaryPath = directory / "crews.world";
		original.saveTo(binaryPath.string());
		auto const reopenedBinary = core::loadWorldDocument(binaryPath);
		reopenedBinary->pauseSimulation();
		require(reopenedBinary->getStationSystemsSnapshot().crews.size() == 1
			&& reopenedBinary->getStationSystemsSnapshot().crews[0].foodConsumed == 0.0,
			"reopened binary crew document did not reconstruct the starting state");
		// A pre-77 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 85", "version: 76");
			auto const stalePath = directory / "stale-crews.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-77 document carrying crews was silently accepted");
		}
		// Malformed crew values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "crewCount: 2", "crewCount: -1");
			auto const malformedPath = directory / "malformed-crew-count.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a negative crew count was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "atmosphereId: habitat", "atmosphereId: ghost");
			auto const malformedPath = directory / "malformed-crew-atmosphere.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a crew breathing in an unknown compartment was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "foodStoreId: food", "foodStoreId: pantry");
			auto const malformedPath = directory / "malformed-crew-food-store.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a crew fed from an unknown store was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "metabolicHeatPerCrewKW: 0.1", "metabolicHeatPerCrewKW: 5.0");
			auto const malformedPath = directory / "malformed-crew-heat.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "metabolic heat beyond the funded food energy was accepted from a file");
		}
	}
}

// Water recovery persistence (#402): the collector and treatment contracts
// round-trip through the document, the runtime reconstructs its recovery
// state from the authored starting conditions on load, and the scenario
// replays identically from the reopened document. A pre-78 document
// carrying the new fields is refused rather than silently dropped, and
// malformed water values in a file are rejected.
void waterFiles(smoke::Context const& context)
{
	namespace filesystem = std::filesystem;
	auto const directory = context.temporaryRoot() / "station-water";
	filesystem::create_directories(directory);
	auto plainStore = [](std::string const& id, double capacity, double starting)
	{
		core::ResourceStoreSettings settings;
		settings.id = id;
		settings.unit = "kg";
		settings.capacity = capacity;
		settings.startingQualified = starting;
		return settings;
	};
	core::AtmosphereSettings habitat;
	habitat.id = "habitat";
	habitat.hazardClass = "normal";
	habitat.volumeM3 = 100.0;
	habitat.startingOxygenKg = 20.0;
	habitat.startingNitrogenKg = 80.0;
	habitat.startingWaterVapourKg = 2.0;
	habitat.startingTemperatureC = 25.0;
	habitat.thermalCapacityKJPerK = 10000.0;
	habitat.minPressureKPa = 1.0;
	habitat.maxPressureKPa = 200.0;
	core::ThermalReservoirSettings sink;
	sink.id = "sink";
	sink.thermalClass = "moderate";
	sink.heatCapacityKJPerK = 10000.0;
	sink.startingTemperatureC = 10.0;
	sink.minTemperatureC = -50.0;
	sink.maxTemperatureC = 200.0;
	core::CoolantLoopSettings loop;
	loop.id = "chilled";
	loop.hazardClass = "normal";
	loop.reservoirId = "sink";
	loop.coolantCapacityKg = 100.0;
	loop.startingCoolantKg = 100.0;
	loop.specificHeatKJPerKgK = 4.0;
	loop.startingTemperatureC = 5.0;
	loop.freezeProtectionSetpointC = 0.0;
	loop.pumpPowerKW = 0.1;
	loop.ratedFlowKgPerS = 10.0;
	loop.exchangerConductanceKWPerK = 0.0;
	loop.heaterCapacityKW = 0.0;
	core::SolarArraySettings field;
	field.id = "field";
	field.capacityKW = 10.0;
	core::WaterCollectorSettings collector;
	collector.id = "cond";
	collector.hazardClass = "normal";
	collector.atmosphereId = "habitat";
	collector.coolingLoopId = "chilled";
	collector.thermalClass = "moderate";
	collector.destStoreId = "raw";
	collector.maxRateKgPerS = 0.01;
	collector.targetRelativeHumidity = 0.5;
	core::WaterTreatmentSettings treatment;
	treatment.id = "plant";
	treatment.hazardClass = "normal";
	treatment.sourceStoreId = "raw";
	treatment.treatedStoreId = "treated";
	treatment.concentrateStoreId = "concentrate";
	treatment.consumableStoreId = "consumable";
	treatment.throughputKgPerS = 0.005;
	treatment.recoveryRatio = 0.8;
	treatment.specificEnergyKWhPerKg = 0.5;
	treatment.consumablePerKg = 0.01;
	treatment.priority = 3;
	core::World original("Water Round Trip", 2, 2);
	original.pauseSimulation();
	original.finishBuild();
	core::StationSystemsSettings settings;
	settings.enabled = true;
	settings.observationIntervalSeconds = 2.0f;
	settings.stores.push_back(plainStore("raw", 100.0, 50.0));
	settings.stores.push_back(plainStore("treated", 100.0, 0.0));
	settings.stores.push_back(plainStore("concentrate", 100.0, 0.0));
	settings.stores.push_back(plainStore("consumable", 100.0, 10.0));
	settings.atmospheres.push_back(habitat);
	settings.thermalReservoirs.push_back(sink);
	settings.coolantLoops.push_back(loop);
	settings.solarArrays.push_back(field);
	settings.waterCollectors.push_back(collector);
	settings.waterTreatments.push_back(treatment);
	std::string diagnostic;
	require(original.configureStationSystems(settings, false, &diagnostic),
		"Water recovery settings edit failed: " + diagnostic);
	original.resumeSimulation();
	require(original.advanceTicks(1800), "Water original run failed");
	auto const expected = original.getStationSystemsSnapshot();
	require(expected.waterCollectors.size() == 1
		&& expected.waterTreatments.size() == 1
		&& expected.waterCollectors[0].collectedKg > 0.0
		&& expected.waterTreatments[0].processedKg > 0.0
		&& expected.waterTreatments[0].acceptedKg > 0.0
		&& expected.waterTreatments[0].concentrateKg > 0.0,
		"Water scenario did not exercise collection and treatment before saving");
	auto const path = directory / "water.world.yaml";
	original.saveTo(path.string());
	require(readFile(path).find("waterCollectors:") != std::string::npos,
		"saved document lacks waterCollectors");
	require(readFile(path).find("waterTreatments:") != std::string::npos,
		"saved document lacks waterTreatments");
	require(readFile(path).find("targetRelativeHumidity") != std::string::npos
		&& readFile(path).find("recoveryRatio") != std::string::npos,
		"saved document lacks the water recovery contract");
	auto const reopened = core::loadWorldDocument(path);
	reopened->pauseSimulation();
	require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
		"authored water recovery settings did not survive the file round trip");
	require(reopened->getStationSystemsSnapshot().waterCollectors[0].collectedKg == 0.0
		&& reopened->getStationSystemsSnapshot().waterTreatments[0].processedKg == 0.0
		&& reopened->getStationSystemsSnapshot().stores[1].currentQualified == 0.0,
		"reopened World did not reconstruct the water recovery starting state");
	reopened->resumeSimulation();
	require(reopened->advanceTicks(1800), "Water reopened run failed");
	require(reopened->getStationSystemsSnapshot() == expected,
		"Water scenario is not reproducible from the document");
	require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
		"Water event history is not reproducible from the document");
	auto const binaryPath = directory / "water.world";
	original.saveTo(binaryPath.string());
	auto const reopenedBinary = core::loadWorldDocument(binaryPath);
	reopenedBinary->pauseSimulation();
	require(reopenedBinary->getStationSystemsSnapshot().waterCollectors.size() == 1
		&& reopenedBinary->getStationSystemsSnapshot().waterTreatments[0].processedKg == 0.0,
		"reopened binary water document did not reconstruct the starting state");
	auto const withField = serializeYaml(original);
	// A pre-78 document carrying the new fields is refused.
	{
		auto stale = withField;
		replaceAll(stale, "version: 85", "version: 77");
		auto const stalePath = directory / "stale-water.world.yaml";
		writeFile(stalePath, stale);
		bool refused = false;
		try { (void)core::loadWorldDocument(stalePath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-78 document carrying water recovery was silently accepted");
	}
	// Malformed water values in a file are rejected.
	{
		auto malformed = withField;
		replaceAll(malformed, "recoveryRatio: 0.8", "recoveryRatio: 0.0");
		auto const malformedPath = directory / "malformed-recovery-ratio.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a zero recovery ratio was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "targetRelativeHumidity: 0.5", "targetRelativeHumidity: 1.5");
		auto const malformedPath = directory / "malformed-target-humidity.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a humidity target above one was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "coolingLoopId: chilled", "coolingLoopId: ghost");
		auto const malformedPath = directory / "malformed-collector-loop.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a collector on an unknown coolant loop was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "hazardClass: normal", "hazardClass: plasma");
		auto const malformedPath = directory / "malformed-water-hazard.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "an undeclared water hazard class was accepted from a file");
	}
}

void gasFiles(smoke::Context const& context)
{
	namespace filesystem = std::filesystem;
	auto const directory = context.temporaryRoot() / "station-gas";
	filesystem::create_directories(directory);
	auto plainStore = [](std::string const& id, double capacity, double starting,
		double protectedReserve = 0.0)
	{
		core::ResourceStoreSettings settings;
		settings.id = id;
		settings.unit = "kg";
		settings.capacity = capacity;
		settings.startingQualified = starting;
		settings.protectedReserve = protectedReserve;
		return settings;
	};
	core::AtmosphereSettings habitat;
	habitat.id = "habitat";
	habitat.hazardClass = "normal";
	habitat.volumeM3 = 100.0;
	habitat.startingOxygenKg = 20.0;
	habitat.startingNitrogenKg = 80.0;
	habitat.startingCarbonDioxideKg = 1.0;
	habitat.startingTemperatureC = 25.0;
	habitat.thermalCapacityKJPerK = 10000.0;
	habitat.minPressureKPa = 1.0;
	habitat.maxPressureKPa = 200.0;
	core::ThermalReservoirSettings sink;
	sink.id = "sink";
	sink.thermalClass = "low";
	sink.heatCapacityKJPerK = 10000.0;
	sink.startingTemperatureC = 10.0;
	sink.minTemperatureC = -50.0;
	sink.maxTemperatureC = 200.0;
	core::SolarArraySettings field;
	field.id = "field";
	field.capacityKW = 100.0;
	core::ElectrolyzerSettings cell;
	cell.id = "cell";
	cell.hazardClass = "normal";
	cell.waterStoreId = "water";
	cell.oxygenStoreId = "o2";
	cell.hydrogenStoreId = "h2";
	cell.thermalClass = "low";
	cell.heatSinkId = "sink";
	cell.rateKgPerS = 0.001;
	cell.specificEnergyKWhPerKgWater = 4.5;
	cell.maxSinkTemperatureC = 60.0;
	cell.priority = 2;
	core::Co2CaptureSettings capture;
	capture.id = "scrub";
	capture.hazardClass = "normal";
	capture.atmosphereId = "habitat";
	capture.captureStoreId = "captured";
	capture.consumableStoreId = "scrubberMedia";
	capture.maxRateKgPerS = 0.001;
	capture.specificEnergyKWhPerKg = 0.5;
	capture.consumablePerKg = 0.01;
	capture.priority = 1;
	core::World original("Gas Round Trip", 2, 2);
	original.pauseSimulation();
	original.finishBuild();
	core::StationSystemsSettings settings;
	settings.enabled = true;
	settings.observationIntervalSeconds = 2.0f;
	settings.stores.push_back(plainStore("water", 100.0, 50.0, 10.0));
	settings.stores.push_back(plainStore("o2", 100.0, 0.0));
	settings.stores.push_back(plainStore("h2", 100.0, 0.0));
	settings.stores.push_back(plainStore("captured", 100.0, 0.0));
	settings.stores.push_back(plainStore("scrubberMedia", 100.0, 10.0));
	settings.atmospheres.push_back(habitat);
	settings.thermalReservoirs.push_back(sink);
	settings.solarArrays.push_back(field);
	settings.electrolyzers.push_back(cell);
	settings.co2Captures.push_back(capture);
	std::string diagnostic;
	require(original.configureStationSystems(settings, false, &diagnostic),
		"Gas production settings edit failed: " + diagnostic);
	original.resumeSimulation();
	require(original.advanceTicks(1800), "Gas original run failed");
	auto const expected = original.getStationSystemsSnapshot();
	require(expected.electrolyzers.size() == 1 && expected.co2Captures.size() == 1
		&& expected.electrolyzers[0].waterSplitKg > 0.0
		&& expected.electrolyzers[0].oxygenProducedKg > 0.0
		&& expected.electrolyzers[0].hydrogenProducedKg > 0.0
		&& expected.electrolyzers[0].heatRoutedKJ > 0.0
		&& expected.co2Captures[0].capturedKg > 0.0
		&& expected.co2Captures[0].consumablesUsed > 0.0,
		"Gas scenario did not exercise electrolysis and capture before saving");
	auto const path = directory / "gas.world.yaml";
	original.saveTo(path.string());
	require(readFile(path).find("electrolyzers:") != std::string::npos,
		"saved document lacks electrolyzers");
	require(readFile(path).find("co2Captures:") != std::string::npos,
		"saved document lacks co2Captures");
	require(readFile(path).find("specificEnergyKWhPerKgWater") != std::string::npos
		&& readFile(path).find("maxSinkTemperatureC") != std::string::npos,
		"saved document lacks the gas production contract");
	auto const reopened = core::loadWorldDocument(path);
	reopened->pauseSimulation();
	require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
		"authored gas production settings did not survive the file round trip");
	require(reopened->getStationSystemsSnapshot().electrolyzers[0].waterSplitKg == 0.0
		&& reopened->getStationSystemsSnapshot().co2Captures[0].capturedKg == 0.0
		&& reopened->getStationSystemsSnapshot().stores[1].currentQualified == 0.0,
		"reopened World did not reconstruct the gas production starting state");
	reopened->resumeSimulation();
	require(reopened->advanceTicks(1800), "Gas reopened run failed");
	require(reopened->getStationSystemsSnapshot() == expected,
		"Gas scenario is not reproducible from the document");
	require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
		"Gas event history is not reproducible from the document");
	auto const binaryPath = directory / "gas.world";
	original.saveTo(binaryPath.string());
	auto const reopenedBinary = core::loadWorldDocument(binaryPath);
	reopenedBinary->pauseSimulation();
	require(reopenedBinary->getStationSystemsSnapshot().electrolyzers.size() == 1
		&& reopenedBinary->getStationSystemsSnapshot().co2Captures[0].capturedKg == 0.0,
		"reopened binary gas document did not reconstruct the starting state");
	auto const withField = serializeYaml(original);
	// A pre-79 document carrying the new fields is refused.
	{
		auto stale = withField;
		replaceAll(stale, "version: 85", "version: 78");
		auto const stalePath = directory / "stale-gas.world.yaml";
		writeFile(stalePath, stale);
		bool refused = false;
		try { (void)core::loadWorldDocument(stalePath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-79 document carrying gas production was silently accepted");
	}
	// Malformed gas values in a file are rejected.
	{
		auto malformed = withField;
		replaceAll(malformed, "specificEnergyKWhPerKgWater: 4.5",
			"specificEnergyKWhPerKgWater: 1.0");
		auto const malformedPath = directory / "malformed-dose.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a dose below the retained chemical energy was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "maxSinkTemperatureC: 60", "maxSinkTemperatureC: -300");
		auto const malformedPath = directory / "malformed-sink-temp.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a sink temperature below absolute zero was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "heatSinkId: sink", "heatSinkId: ghost");
		auto const malformedPath = directory / "malformed-heat-sink.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "an electrolyzer on an unknown heat sink was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "captureStoreId: captured", "captureStoreId: ghost");
		auto const malformedPath = directory / "malformed-capture-store.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a capture machine with an unknown capture store was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "hazardClass: normal", "hazardClass: plasma");
		auto const malformedPath = directory / "malformed-gas-hazard.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "an undeclared gas hazard class was accepted from a file");
	}
}

void fuelCellFiles(smoke::Context const& context)
{
	namespace filesystem = std::filesystem;
	auto const directory = context.temporaryRoot() / "station-fuelcells";
	filesystem::create_directories(directory);
	auto plainStore = [](std::string const& id, double capacity, double starting,
		double protectedReserve = 0.0)
	{
		core::ResourceStoreSettings settings;
		settings.id = id;
		settings.unit = "kg";
		settings.capacity = capacity;
		settings.startingQualified = starting;
		settings.protectedReserve = protectedReserve;
		return settings;
	};
	core::ThermalReservoirSettings sink;
	sink.id = "sink";
	sink.thermalClass = "low";
	sink.heatCapacityKJPerK = 10000.0;
	sink.startingTemperatureC = 10.0;
	sink.minTemperatureC = -50.0;
	sink.maxTemperatureC = 200.0;
	core::FuelCellSettings cell;
	cell.id = "backup";
	cell.hazardClass = "normal";
	cell.hydrogenStoreId = "h2";
	cell.oxygenStoreId = "o2";
	cell.waterStoreId = "productWater";
	cell.thermalClass = "low";
	cell.heatSinkId = "sink";
	cell.ratedOutputKW = 10.0;
	cell.efficiency = 0.6;
	cell.commandedOutputKW = 6.0;
	cell.maxSinkTemperatureC = 80.0;
	core::World original("FuelCell Round Trip", 2, 2);
	original.pauseSimulation();
	original.finishBuild();
	core::StationSystemsSettings settings;
	settings.enabled = true;
	settings.observationIntervalSeconds = 2.0f;
	settings.stores.push_back(plainStore("h2", 10.0, 10.0));
	settings.stores.push_back(plainStore("o2", 100.0, 100.0));
	settings.stores.push_back(plainStore("productWater", 200.0, 0.0));
	settings.thermalReservoirs.push_back(sink);
	settings.fuelCells.push_back(cell);
	std::string diagnostic;
	require(original.configureStationSystems(settings, false, &diagnostic),
		"Fuel-cell backup settings edit failed: " + diagnostic);
	original.resumeSimulation();
	require(original.advanceTicks(1800), "FuelCell original run failed");
	auto const expected = original.getStationSystemsSnapshot();
	require(expected.fuelCells.size() == 1
		&& expected.fuelCells[0].electricalKJ > 0.0
		&& expected.fuelCells[0].hydrogenConsumedKg > 0.0
		&& expected.fuelCells[0].waterProducedKg > 0.0
		&& expected.fuelCells[0].wasteHeatKJ > 0.0
		&& expected.fuelCells[0].enduranceSeconds > 0.0,
		"Fuel-cell scenario did not exercise the backup before saving");
	auto const path = directory / "fuelcells.world.yaml";
	original.saveTo(path.string());
	require(readFile(path).find("fuelCells:") != std::string::npos,
		"saved document lacks fuelCells");
	require(readFile(path).find("hydrogenStoreId") != std::string::npos
		&& readFile(path).find("commandedOutputKW") != std::string::npos
		&& readFile(path).find("maxSinkTemperatureC") != std::string::npos,
		"saved document lacks the fuel-cell backup contract");
	require(readFile(path).find("endurance") == std::string::npos,
		"saved document leaked runtime endurance state");
	auto const reopened = core::loadWorldDocument(path);
	reopened->pauseSimulation();
	require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
		"authored fuel-cell backup settings did not survive the file round trip");
	require(reopened->getStationSystemsSnapshot().fuelCells[0].hydrogenConsumedKg == 0.0
		&& reopened->getStationSystemsSnapshot().fuelCells[0].electricalKJ == 0.0
		&& reopened->getStationSystemsSnapshot().stores[0].currentQualified == 10.0,
		"reopened World did not reconstruct the fuel-cell starting state");
	reopened->resumeSimulation();
	require(reopened->advanceTicks(1800), "FuelCell reopened run failed");
	require(reopened->getStationSystemsSnapshot() == expected,
		"Fuel-cell scenario is not reproducible from the document");
	require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
		"Fuel-cell event history is not reproducible from the document");
	auto const binaryPath = directory / "fuelcells.world";
	original.saveTo(binaryPath.string());
	auto const reopenedBinary = core::loadWorldDocument(binaryPath);
	reopenedBinary->pauseSimulation();
	require(reopenedBinary->getStationSystemsSnapshot().fuelCells.size() == 1
		&& reopenedBinary->getStationSystemsSnapshot().fuelCells[0].hydrogenConsumedKg == 0.0,
		"reopened binary fuel-cell document did not reconstruct the starting state");
	auto const withField = serializeYaml(original);
	// A pre-80 document carrying the new fields is refused.
	{
		auto stale = withField;
		replaceAll(stale, "version: 85", "version: 79");
		auto const stalePath = directory / "stale-fuelcells.world.yaml";
		writeFile(stalePath, stale);
		bool refused = false;
		try { (void)core::loadWorldDocument(stalePath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-80 document carrying fuel-cell backup was silently accepted");
	}
	// Malformed fuel-cell values in a file are rejected.
	{
		auto malformed = withField;
		replaceAll(malformed, "efficiency: 0.6", "efficiency: 1.5");
		auto const malformedPath = directory / "malformed-efficiency.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "an over-unity fuel-cell efficiency was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "commandedOutputKW: 6", "commandedOutputKW: 12");
		auto const malformedPath = directory / "malformed-command.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a commanded output above the installed rating was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "heatSinkId: sink", "heatSinkId: ghost");
		auto const malformedPath = directory / "malformed-cell-sink.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a fuel cell on an unknown heat sink was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "hazardClass: normal", "hazardClass: plasma");
		auto const malformedPath = directory / "malformed-cell-hazard.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "an undeclared fuel-cell hazard class was accepted from a file");
	}
}

void carbonRecoveryFiles(smoke::Context const& context)
{
	namespace filesystem = std::filesystem;
	auto const directory = context.temporaryRoot() / "station-carbon";
	filesystem::create_directories(directory);
	auto plainStore = [](std::string const& id, double capacity, double starting,
		double protectedReserve = 0.0)
	{
		core::ResourceStoreSettings settings;
		settings.id = id;
		settings.unit = "kg";
		settings.capacity = capacity;
		settings.startingQualified = starting;
		settings.protectedReserve = protectedReserve;
		return settings;
	};
	core::ThermalReservoirSettings sink;
	sink.id = "sink";
	sink.thermalClass = "low";
	sink.heatCapacityKJPerK = 10000.0;
	sink.startingTemperatureC = 10.0;
	sink.minTemperatureC = -50.0;
	sink.maxTemperatureC = 200.0;
	core::SolarArraySettings field;
	field.id = "field";
	field.capacityKW = 100.0;
	core::SabatierReactorSettings reactor;
	reactor.id = "sabatier";
	reactor.hazardClass = "normal";
	reactor.co2StoreId = "co2";
	reactor.hydrogenStoreId = "h2";
	reactor.methaneStoreId = "methane";
	reactor.waterStoreId = "rawWater";
	reactor.thermalClass = "low";
	reactor.heatSinkId = "sink";
	reactor.rateKgPerS = 0.001;
	reactor.specificEnergyKWhPerKgCO2 = 1.0;
	reactor.maxSinkTemperatureC = 60.0;
	reactor.priority = 2;
	core::MethaneProcessorSettings processor;
	processor.id = "pyrolysis";
	processor.hazardClass = "normal";
	processor.methaneStoreId = "methane";
	processor.hydrogenStoreId = "h2";
	processor.carbonStoreId = "carbon";
	processor.thermalClass = "low";
	processor.heatSinkId = "sink";
	processor.rateKgPerS = 0.0003;
	processor.specificEnergyKWhPerKgCH4 = 2.0;
	processor.maxSinkTemperatureC = 60.0;
	processor.priority = 1;
	core::World original("Carbon Round Trip", 2, 2);
	original.pauseSimulation();
	original.finishBuild();
	core::StationSystemsSettings settings;
	settings.enabled = true;
	settings.observationIntervalSeconds = 2.0f;
	settings.stores.push_back(plainStore("co2", 100.0, 10.0));
	settings.stores.push_back(plainStore("h2", 100.0, 10.0));
	settings.stores.push_back(plainStore("methane", 100.0, 0.0));
	settings.stores.push_back(plainStore("rawWater", 100.0, 0.0));
	settings.stores.push_back(plainStore("carbon", 100.0, 0.0));
	settings.thermalReservoirs.push_back(sink);
	settings.solarArrays.push_back(field);
	settings.sabatierReactors.push_back(reactor);
	settings.methaneProcessors.push_back(processor);
	std::string diagnostic;
	require(original.configureStationSystems(settings, false, &diagnostic),
		"Carbon recovery settings edit failed: " + diagnostic);
	original.resumeSimulation();
	require(original.advanceTicks(1800), "Carbon original run failed");
	auto const expected = original.getStationSystemsSnapshot();
	require(expected.sabatierReactors.size() == 1 && expected.methaneProcessors.size() == 1
		&& expected.sabatierReactors[0].co2ConsumedKg > 0.0
		&& expected.sabatierReactors[0].methaneProducedKg > 0.0
		&& expected.sabatierReactors[0].waterProducedKg > 0.0
		&& expected.sabatierReactors[0].heatRoutedKJ > 0.0
		&& expected.methaneProcessors[0].methaneConsumedKg > 0.0
		&& expected.methaneProcessors[0].hydrogenReturnedKg > 0.0
		&& expected.methaneProcessors[0].carbonProducedKg > 0.0,
		"Carbon scenario did not exercise both stages before saving");
	auto const path = directory / "carbon.world.yaml";
	original.saveTo(path.string());
	require(readFile(path).find("sabatierReactors:") != std::string::npos,
		"saved document lacks sabatierReactors");
	require(readFile(path).find("methaneProcessors:") != std::string::npos,
		"saved document lacks methaneProcessors");
	require(readFile(path).find("specificEnergyKWhPerKgCO2") != std::string::npos
		&& readFile(path).find("specificEnergyKWhPerKgCH4") != std::string::npos
		&& readFile(path).find("co2StoreId") != std::string::npos
		&& readFile(path).find("carbonStoreId") != std::string::npos,
		"saved document lacks the carbon-recovery contract");
	auto const reopened = core::loadWorldDocument(path);
	reopened->pauseSimulation();
	require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
		"authored carbon-recovery settings did not survive the file round trip");
	require(reopened->getStationSystemsSnapshot().sabatierReactors[0].co2ConsumedKg == 0.0
		&& reopened->getStationSystemsSnapshot().methaneProcessors[0].methaneConsumedKg == 0.0
		&& reopened->getStationSystemsSnapshot().stores[2].currentQualified == 0.0,
		"reopened World did not reconstruct the carbon-recovery starting state");
	reopened->resumeSimulation();
	require(reopened->advanceTicks(1800), "Carbon reopened run failed");
	require(reopened->getStationSystemsSnapshot() == expected,
		"Carbon scenario is not reproducible from the document");
	require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
		"Carbon event history is not reproducible from the document");
	auto const binaryPath = directory / "carbon.world";
	original.saveTo(binaryPath.string());
	auto const reopenedBinary = core::loadWorldDocument(binaryPath);
	reopenedBinary->pauseSimulation();
	require(reopenedBinary->getStationSystemsSnapshot().sabatierReactors.size() == 1
		&& reopenedBinary->getStationSystemsSnapshot().methaneProcessors.size() == 1
		&& reopenedBinary->getStationSystemsSnapshot().sabatierReactors[0].co2ConsumedKg == 0.0,
		"reopened binary carbon document did not reconstruct the starting state");
	auto const withField = serializeYaml(original);
	// A pre-83 document carrying the new fields is refused.
	{
		auto stale = withField;
		replaceAll(stale, "version: 85", "version: 82");
		auto const stalePath = directory / "stale-carbon.world.yaml";
		writeFile(stalePath, stale);
		bool refused = false;
		try { (void)core::loadWorldDocument(stalePath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-83 document carrying carbon recovery was silently accepted");
	}
	// A methane dose below the declared endothermic reaction heat is
	// refused from a file.
	{
		auto malformed = withField;
		replaceAll(malformed, "specificEnergyKWhPerKgCH4: 2", "specificEnergyKWhPerKgCH4: 1");
		auto const malformedPath = directory / "malformed-carbon-dose.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a methane dose below the reaction demand was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "co2StoreId: co2", "co2StoreId: ghost");
		auto const malformedPath = directory / "malformed-co2-store.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a reactor with an unknown CO2 store was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "carbonStoreId: carbon", "carbonStoreId: ghost");
		auto const malformedPath = directory / "malformed-carbon-store.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a processor with an unknown carbon store was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "maxSinkTemperatureC: 60", "maxSinkTemperatureC: -300");
		auto const malformedPath = directory / "malformed-carbon-sink-temp.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a carbon-recovery sink temperature below absolute zero was accepted from a file");
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "hazardClass: normal", "hazardClass: plasma");
		auto const malformedPath = directory / "malformed-carbon-hazard.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "an undeclared carbon-recovery hazard class was accepted from a file");
	}
}

void nutrientRecoveryFiles(smoke::Context const& context)
{
	namespace filesystem = std::filesystem;
	auto const directory = context.temporaryRoot() / "station-nutrient";
	filesystem::create_directories(directory);
	auto plainStore = [](std::string const& id, double capacity, double starting,
		double protectedReserve = 0.0)
	{
		core::ResourceStoreSettings settings;
		settings.id = id;
		settings.unit = "kg";
		settings.capacity = capacity;
		settings.startingQualified = starting;
		settings.protectedReserve = protectedReserve;
		return settings;
	};
	core::ResourceCompositionSettings feedComposition;
	feedComposition.moisture = 0.9;
	feedComposition.nutrientNitrogen = 0.05;
	feedComposition.atmosphericNitrogen = 0.02;
	feedComposition.phosphorus = 0.02;
	feedComposition.potassium = 0.02;
	feedComposition.otherMinerals = 0.03;
	feedComposition.energyKJPerKgDry = 8000.0;
	core::ThermalReservoirSettings sink;
	sink.id = "sink";
	sink.thermalClass = "low";
	sink.heatCapacityKJPerK = 10000.0;
	sink.startingTemperatureC = 20.0;
	sink.minTemperatureC = -50.0;
	sink.maxTemperatureC = 200.0;
	core::SolarArraySettings field;
	field.id = "field";
	field.capacityKW = 100.0;
	core::MicrobialProcessorSettings processor;
	processor.id = "digester";
	processor.hazardClass = "normal";
	processor.feedStoreId = "waste";
	processor.oxygenStoreId = "oxygen";
	processor.waterStoreId = "processWater";
	processor.consumableStoreId = "media";
	processor.nutrientStoreId = "nutrients";
	processor.recoveredWaterStoreId = "recoveredWater";
	processor.co2StoreId = "co2";
	processor.residueStoreId = "residue";
	processor.thermalClass = "low";
	processor.heatSinkId = "sink";
	processor.rateKgPerS = 0.001;
	processor.specificEnergyKWhPerKgFeed = 2.0;
	processor.oxygenPerKgFeedKg = 0.8;
	processor.processWaterPerKgFeedKg = 0.5;
	processor.consumablePerKgFeed = 0.01;
	processor.co2PerKgFeedKg = 0.3;
	processor.nitrogenRecovery = 0.8;
	processor.phosphorusRecovery = 0.8;
	processor.potassiumRecovery = 0.8;
	processor.mineralsRecovery = 0.7;
	processor.waterRecovery = 0.7;
	processor.reactionHeatKJPerKgFeed = 600.0;
	processor.minOperatingTemperatureC = 15.0;
	processor.maxSinkTemperatureC = 45.0;
	// #408 condition/recovery contract, with non-default values so the
	// file round trip exercises the new fields.
	processor.initialCondition = 0.75;
	processor.starvationDegradePerHour = 0.04;
	processor.consumableExhaustionDegradePerHour = 0.06;
	processor.coldStressDegradePerHour = 0.03;
	processor.heatStressDegradePerHour = 0.12;
	processor.recoveryPerHour = 0.03;
	processor.stockStoreId = "stock";
	processor.stockUnitsPerConditionPoint = 5.0;
	processor.priority = 1;
	core::World original("Nutrient Round Trip", 2, 2);
	original.pauseSimulation();
	original.finishBuild();
	core::StationSystemsSettings settings;
	settings.enabled = true;
	settings.observationIntervalSeconds = 2.0f;
	settings.stores.push_back(plainStore("waste", 100.0, 10.0));
	settings.stores.back().composition = feedComposition;
	settings.stores.push_back(plainStore("oxygen", 100.0, 10.0));
	settings.stores.push_back(plainStore("processWater", 100.0, 10.0));
	settings.stores.push_back(plainStore("media", 100.0, 10.0));
	settings.stores.push_back(plainStore("nutrients", 100.0, 0.0));
	settings.stores.push_back(plainStore("recoveredWater", 100.0, 0.0));
	settings.stores.push_back(plainStore("co2", 100.0, 0.0));
	settings.stores.push_back(plainStore("residue", 100.0, 0.0));
	settings.stores.push_back(plainStore("stock", 10.0, 1.0));
	settings.thermalReservoirs.push_back(sink);
	settings.solarArrays.push_back(field);
	settings.microbialProcessors.push_back(processor);
	std::string diagnostic;
	require(original.configureStationSystems(settings, false, &diagnostic),
		"Nutrient recovery settings edit failed: " + diagnostic);
	original.resumeSimulation();
	require(original.advanceTicks(1800), "Nutrient original run failed");
	auto const expected = original.getStationSystemsSnapshot();
	require(expected.microbialProcessors.size() == 1
		&& expected.microbialProcessors[0].feedConsumedKg > 0.0
		&& expected.microbialProcessors[0].nitrogenRecoveredKg > 0.0
		&& expected.microbialProcessors[0].recoveredWaterKg > 0.0
		&& expected.microbialProcessors[0].residueProducedKg > 0.0
		&& expected.microbialProcessors[0].heatRoutedKJ > 0.0,
		"Nutrient scenario did not exercise the digestion before saving");
	auto const path = directory / "nutrient.world.yaml";
	original.saveTo(path.string());
	require(readFile(path).find("microbialProcessors:") != std::string::npos,
		"saved document lacks microbialProcessors");
	require(readFile(path).find("nitrogenRecovery") != std::string::npos
		&& readFile(path).find("feedStoreId") != std::string::npos
		&& readFile(path).find("recoveredWaterStoreId") != std::string::npos
		&& readFile(path).find("minOperatingTemperatureC") != std::string::npos,
		"saved document lacks the nutrient-recovery contract");
	require(readFile(path).find("initialCondition") != std::string::npos
		&& readFile(path).find("starvationDegradePerHour") != std::string::npos
		&& readFile(path).find("consumableExhaustionDegradePerHour") != std::string::npos
		&& readFile(path).find("coldStressDegradePerHour") != std::string::npos
		&& readFile(path).find("heatStressDegradePerHour") != std::string::npos
		&& readFile(path).find("recoveryPerHour") != std::string::npos
		&& readFile(path).find("stockStoreId") != std::string::npos
		&& readFile(path).find("stockUnitsPerConditionPoint") != std::string::npos,
		"saved document lacks the #408 condition/recovery contract");
	auto const reopened = core::loadWorldDocument(path);
	reopened->pauseSimulation();
	require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
		"authored nutrient-recovery settings did not survive the file round trip");
	require(reopened->getStationSystemsSnapshot().microbialProcessors[0].feedConsumedKg == 0.0
		&& reopened->getStationSystemsSnapshot().microbialProcessors[0].nutrientProducedKg == 0.0,
		"reopened World did not reconstruct the nutrient-recovery starting state");
	reopened->resumeSimulation();
	require(reopened->advanceTicks(1800), "Nutrient reopened run failed");
	require(reopened->getStationSystemsSnapshot() == expected,
		"Nutrient scenario is not reproducible from the document");
	require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
		"Nutrient event history is not reproducible from the document");
	auto const binaryPath = directory / "nutrient.world";
	original.saveTo(binaryPath.string());
	auto const reopenedBinary = core::loadWorldDocument(binaryPath);
	reopenedBinary->pauseSimulation();
	require(reopenedBinary->getStationSystemsSnapshot().microbialProcessors.size() == 1
		&& reopenedBinary->getStationSystemsSnapshot().microbialProcessors[0].feedConsumedKg == 0.0,
		"reopened binary nutrient document did not reconstruct the starting state");
	auto const withField = serializeYaml(original);
	// A pre-85 document carrying the #408 condition/recovery fields is
	// refused rather than silently re-defaulted.
	{
		auto stale = withField;
		replaceAll(stale, "version: 85", "version: 84");
		auto const stalePath = directory / "stale-nutrient.world.yaml";
		writeFile(stalePath, stale);
		bool refused = false;
		try { (void)core::loadWorldDocument(stalePath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-85 document carrying nutrient-recovery condition fields was silently accepted");
	}
	// A recovery yield above 1 is refused from a file.
	{
		auto malformed = withField;
		replaceAll(malformed, "nitrogenRecovery: 0.8", "nitrogenRecovery: 1.8");
		auto const malformedPath = directory / "malformed-nutrient-yield.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a recovery yield above 1 was accepted from a file");
	}
	// A reaction heat above the feedstock's declared chemical energy is
	// refused from a file.
	{
		auto malformed = withField;
		replaceAll(malformed, "reactionHeatKJPerKgFeed: 600", "reactionHeatKJPerKgFeed: 900");
		auto const malformedPath = directory / "malformed-nutrient-heat.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a reaction heat above the feed's chemical energy was accepted from a file");
	}
	// An empty operating window is refused from a file.
	{
		auto malformed = withField;
		replaceAll(malformed, "minOperatingTemperatureC: 15", "minOperatingTemperatureC: 50");
		auto const malformedPath = directory / "malformed-nutrient-window.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "an empty microbial operating window was accepted from a file");
	}
	// An initial culture condition above 1 is refused from a file (#408).
	{
		auto malformed = withField;
		replaceAll(malformed, "initialCondition: 0.75", "initialCondition: 2.5");
		auto const malformedPath = directory / "malformed-nutrient-initial-condition.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "an initial culture condition above 1 was accepted from a file");
	}
	// A negative degradation rate is refused from a file (#408).
	{
		auto malformed = withField;
		replaceAll(malformed, "starvationDegradePerHour: 0.04", "starvationDegradePerHour: -0.04");
		auto const malformedPath = directory / "malformed-nutrient-degrade-rate.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a negative starvation degradation rate was accepted from a file");
	}
	// A declared stock store with zero units per condition point is
	// refused from a file (#408).
	{
		auto malformed = withField;
		replaceAll(malformed, "stockUnitsPerConditionPoint: 5", "stockUnitsPerConditionPoint: 0");
		auto const malformedPath = directory / "malformed-nutrient-stock-rate.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a zero stock units per condition point with a declared stock store was accepted from a file");
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
	checks.push_back({"batteryFiles", batteryFiles});
	checks.push_back({"deliveryFiles", deliveryFiles});
	checks.push_back({"wasteEjectionFiles", wasteEjectionFiles});
	checks.push_back({"thermalFiles", thermalFiles});
	checks.push_back({"bufferFiles", bufferFiles});
	checks.push_back({"coolantFiles", coolantFiles});
	checks.push_back({"chillerFiles", chillerFiles});
	checks.push_back({"generatorFiles", generatorFiles});
	checks.push_back({"atmosphereFiles", atmosphereFiles});
	checks.push_back({"crewFiles", crewFiles});
	checks.push_back({"waterFiles", waterFiles});
	checks.push_back({"gasFiles", gasFiles});
	checks.push_back({"fuelCellFiles", fuelCellFiles});
	checks.push_back({"carbonRecoveryFiles", carbonRecoveryFiles});
	checks.push_back({"nutrientRecoveryFiles", nutrientRecoveryFiles});
}
