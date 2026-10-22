#include "StationRig.h"
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
#include "core/StationConfigurationDocument.h"
#include "core/StationPorts.h"
#include "core/StationReference.h"
#include "core/TransactionalFileWriter.h"
#include <yaml-cpp/yaml.h>
#include "core/AgentTagRegistryDocument.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"

#include <array>
#include <algorithm>
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

	void removeStepRatio(std::string& text)
	{
		auto const at = text.find("  systemsStepWorldTicks:");
		if (at != std::string::npos) text.erase(at, text.find('\n', at) - at + 1);
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

	// #765: ratio persistence and validation share the authored settings path.
	void stepRatioConfiguration(smoke::Context const& context)
	{
		core::World world("Station step ratio", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.systemsStepWorldTicks = 6;
		std::string diagnostic;
		require(world.configureStationSystems(settings, false, &diagnostic), diagnostic);

		core::StationConfigurationDocument document; document.settings = settings;
		auto yaml = core::serializeStationConfiguration(document);
		require(yaml.find("schemaVersion: 106") != std::string::npos
			&& yaml.find("systemsStepWorldTicks: 6") != std::string::npos,
			"Station configuration did not persist its step ratio");
		core::StationConfigurationDocument loaded;
		require(core::parseStationConfiguration(yaml, loaded, &diagnostic)
			&& loaded.settings == settings, "Station step ratio did not round trip: " + diagnostic);

		auto legacy = yaml;
		replaceAll(legacy, "schemaVersion: 106", "schemaVersion: 103");
		replaceAll(legacy, "  systemsStepWorldTicks: 6\n", "");
		require(core::parseStationConfiguration(legacy, loaded, &diagnostic)
			&& loaded.settings.systemsStepWorldTicks == 1,
			"Legacy Station configuration did not default its step ratio to 1");
		auto stale = yaml;
		replaceAll(stale, "schemaVersion: 106", "schemaVersion: 103");
		require(!core::parseStationConfiguration(stale, loaded, &diagnostic)
			&& diagnostic.find("step ratio requires World schema 104") != std::string::npos
			&& loaded.settings.systemsStepWorldTicks == 1,
			"Pre-104 Station configuration silently accepted a step ratio");
		auto invalid = yaml;
		replaceAll(invalid, "systemsStepWorldTicks: 6", "systemsStepWorldTicks: 0");
		require(!core::parseStationConfiguration(invalid, loaded, &diagnostic)
			&& diagnostic.find("systemsStepWorldTicks 0 is outside allowed range [1, 60]") != std::string::npos
			&& loaded.settings.systemsStepWorldTicks == 1,
			"Invalid Station step ratio was accepted or replaced the loaded document");

		world.markSaved();
		world.resumeSimulation();
		auto changed = settings; changed.systemsStepWorldTicks = 7;
		require(!world.configureStationSystems(changed, false, &diagnostic)
			&& diagnostic == "Pause the World before editing Station systems"
			&& world.getStationSystemsSettings() == settings && !world.isModified(),
			"Running World accepted a Station step-ratio change");
		world.pauseSimulation();
		changed.systemsStepWorldTicks = 0;
		require(!world.configureStationSystems(changed, false, &diagnostic)
			&& diagnostic.find("systemsStepWorldTicks 0 is outside allowed range [1, 60]") != std::string::npos
			&& world.getStationSystemsSettings() == settings && !world.isModified(),
			"Invalid Station step-ratio change mutated the World");
		auto const path = context.temporaryRoot() / "station-step-ratio.world.yaml";
		world.saveTo(path.string());
		auto reopened = core::loadWorldDocument(path);
		require(reopened->getStationSystemsSettings() == settings,
			"World document did not persist its Station step ratio");
		reopened->saveTo(path.string());
		auto resaved = core::loadWorldDocument(path);
		require(resaved->getStationSystemsSettings() == settings,
			"Re-saving a loaded World lost its adopted Station step ratio");

		// Loading replaces the World settings with the document's timing rather
		// than retaining the target's current configuration.  The same preflight
		// must leave that target untouched if the saved ratio is invalid.
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData); writer->serialize();
		auto const savedWorld = writer->getSerializedString();
		core::World target("Station ratio target", 2, 2);
		target.pauseSimulation(); target.finishBuild();
		auto targetSettings = settings; targetSettings.systemsStepWorldTicks = 4;
		require(target.configureStationSystems(targetSettings, false, &diagnostic), diagnostic);
		target.markSaved();
		auto reader = core::YamlSerializer::fromString(savedWorld); reader->deserialize();
		require(target.deserialize(*reader, workData)
			&& target.getStationSystemsSettings() == settings,
			"World load retained the target Station step ratio instead of adopting the saved ratio");

		auto invalidWorld = savedWorld;
		replaceAll(invalidWorld, "systemsStepWorldTicks: 6", "systemsStepWorldTicks: 0");
		target.markSaved();
		auto invalidReader = core::YamlSerializer::fromString(invalidWorld); invalidReader->deserialize();
		bool refused = false;
		try { refused = !target.deserialize(*invalidReader, workData); }
		catch (std::exception const&) { refused = true; }
		require(refused && target.getStationSystemsSettings() == settings && !target.isModified(),
			"Out-of-range saved Station step ratio changed the target World");

		auto legacyWorld = savedWorld;
		replaceAll(legacyWorld, "version: 106", "version: 103");
		replaceAll(legacyWorld, "  systemsStepWorldTicks: 6\n", "");
		auto legacyReader = core::YamlSerializer::fromString(legacyWorld); legacyReader->deserialize();
		require(target.deserialize(*legacyReader, workData)
			&& target.getStationSystemsSettings().systemsStepWorldTicks == 1,
			"Pre-change World document did not default its Station step ratio to 1");
	}

	// #699: identity is authored metadata, not a name/unit inference or an
	// allocation rule. Exercise public persistence and atomic configuration.
	void materialIdentity(smoke::Context const& context)
	{
		core::World world("Materials", 2, 2);
		enable(world, 1.0f, {{"oxygen", "kg", 100, 20, 5}, {"water", "kg", 100, 10}});
		auto legacy = world.getStationSystemsSettings();
		legacy.systemsStepWorldTicks = 1; // missing-field legacy contract
		for (auto const& store : legacy.stores)
			require(store.materialId.empty(), "Store name or unit inferred a material");
		core::StationConfigurationDocument document;
		document.settings = legacy;
		core::StationConfigurationDocument loaded;
		std::string diagnostic;
		for (auto const version : {96, 97, 98})
		{
			auto yaml = core::serializeStationConfiguration(document);
			replaceAll(yaml, "schemaVersion: 106", "schemaVersion: " + std::to_string(version));
			removeStepRatio(yaml);
			require(core::parseStationConfiguration(yaml, loaded, &diagnostic)
				&& loaded.settings == legacy, "Legacy standalone material was not unclassified");
		}
		auto const path = context.temporaryRoot() / "material.world.yaml";
		auto yaml = serializeYaml(world);
		replaceAll(yaml, "version: 106", "version: 97");
		removeStepRatio(yaml);
		writeFile(path, yaml);
		auto reopened = core::loadWorldDocument(path);
		require(reopened->getStationSystemsSettings() == legacy && !reopened->isModified()
			&& readFile(path) == yaml, "Legacy World read changed identity, dirty state or file");

		auto explicitSettings = legacy;
		explicitSettings.stores[0].materialId = "water"; // deliberately misleading store name
		explicitSettings.stores[0].protectedReserve = 6;
		explicitSettings.stores[1].materialId = "custom-feedstock-2";
		explicitSettings.stores[1].composition = core::ResourceCompositionSettings{};
		require(world.configureStationSystems(explicitSettings, false, &diagnostic), diagnostic);
		document.settings = explicitSettings;
		core::saveStationConfiguration((context.temporaryRoot() / "material.station.yaml").string(), document);
		require(core::loadStationConfiguration((context.temporaryRoot() / "material.station.yaml").string(), loaded,
			&diagnostic) && loaded.settings == explicitSettings, "Standalone identity did not round trip");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const file = context.temporaryRoot() / (std::string("explicit-material") + suffix);
			world.saveTo(file.string());
			auto restored = core::loadWorldDocument(file);
			require(restored->getStationSystemsSettings() == explicitSettings && !restored->isModified(),
				"World identity did not round trip");
			restored->resetSimulation();
			require(restored->getStationSystemsSettings() == explicitSettings, "Reset lost material identity");
			restored->resumeSimulation(); require(smoke::station::advanceSeconds(restored, 1.0), "Restored inventory observation failed");
			restored->pauseSimulation();
			auto const usable = restored->queryStationSystemsChart("stored-usable/water/kg/aggregate");
			auto const hazardous = restored->queryStationSystemsChart("stored-usable/custom-feedstock-2/kg/aggregate");
			require(!usable.incomplete && usable.series[0].values[0] == 20
				&& !hazardous.incomplete && hazardous.series[0].values[0] == 10,
				"Restored qualified inventory lost identity/hazard/reserve semantics");
		}
		require(world.rebuildTraversalTopology()
			&& world.getStationSystemsSettings() == explicitSettings, "Structural replay lost material identity");
		world.markSaved();
		auto const snapshot = world.getStationSystemsSnapshot();
		for (auto const& invalid : std::vector<std::string>{"Water", " water", "a b", "water/ice",
			"-water", "water-", "water--ice", "2water", std::string(65, 'a'), std::string("water\0ice", 9)})
		{
			auto bad = explicitSettings;
			bad.stores[0].materialId = invalid;
			require(!world.configureStationSystems(bad, false, &diagnostic)
				&& diagnostic.find("material identity") != std::string::npos
				&& world.getStationSystemsSettings() == explicitSettings
				&& world.getStationSystemsSnapshot() == snapshot && !world.isModified(),
				"Invalid identity changed World state");
			auto malformed = document;
			malformed.settings = bad;
			require(!core::parseStationConfiguration(core::serializeStationConfiguration(malformed), loaded, &diagnostic)
				&& loaded.settings == explicitSettings, "Invalid standalone identity partially replaced document");
			auto badYaml = YAML::Load(serializeYaml(world));
			badYaml["stationSystems"]["stores"][0]["materialId"] = invalid;
			writeFile(path, YAML::Dump(badYaml));
			bool refused = false;
			try { reopened = core::loadWorldDocument(path); }
			catch (std::exception const&) { refused = true; }
			require(refused && reopened->getStationSystemsSettings() == legacy && !reopened->isModified(),
				"Malformed World identity replaced the caller's loaded document");
		}
		auto stale = core::serializeStationConfiguration(document);
		replaceAll(stale, "schemaVersion: 106", "schemaVersion: 97");
		require(!core::parseStationConfiguration(stale, loaded, &diagnostic)
			&& loaded.settings == explicitSettings, "Pre-98 document silently accepted explicit identity");
		// Labels never change supply or hazard compatibility. Compare runs
		// with identical physical settings and only metadata changed.
		auto unclassified = explicitSettings;
		for (auto& store : unclassified.stores) store.materialId.clear();
		core::World baseline("Baseline", 2, 2);
		baseline.pauseSimulation(); baseline.finishBuild();
		require(baseline.configureStationSystems(unclassified, false, &diagnostic), diagnostic);
		world.resumeSimulation(); baseline.resumeSimulation();
		require(smoke::station::advanceSeconds(world, 1.0) && smoke::station::advanceSeconds(baseline, 1.0)
			&& world.getStationSystemsSnapshot() == baseline.getStationSystemsSnapshot(),
			"Material labels changed simulation allocation");
	}

	void configurationFile(smoke::Context const& context)
	{
		auto const path = context.temporaryRoot() / "draft.station.yaml";
		core::StationConfigurationDocument empty;
		core::StationConfigurationDocument blank;
		std::string diagnostic;
		require(core::parseStationConfiguration(core::serializeStationConfiguration(empty), blank,
			&diagnostic) && blank.settings == empty.settings,
			"Empty Station configuration did not round trip");
		core::StationConfigurationDocument original;
		original.settings.enabled = true;
		original.settings.stores.push_back({"supply", "kg", 100.0, 25.0});
		original.settings.transfers.push_back({"pump", "supply", "missing", "kg", 1.0, 1});
		original.positions["Store:supply:0"] = {135.0f, -42.0f};
		original.panX = -12; original.panY = 78; original.zoom = 0.6f;
		core::saveStationConfiguration(path.string(), original);
		auto const bytes = readFile(path);
		core::StationConfigurationDocument restored;
		require(core::loadStationConfiguration(path.string(), restored, &diagnostic), diagnostic);
		require(restored.settings == original.settings && restored.positions == original.positions
			&& restored.panX == original.panX && restored.panY == original.panY
			&& restored.zoom == original.zoom, "Configuration round trip lost authored data or layout");
		require(bytes == core::serializeStationConfiguration(restored),
			"Station configuration YAML changed during round trip");
		core::World world("Portable", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		std::string ignored;
		auto valid = original.settings; valid.transfers.clear();
		require(world.configureStationSystems(valid, false, &ignored), ignored);
		auto const worldYaml = YAML::Load(serializeYaml(world));
		auto comparable = original; comparable.settings = valid;
		auto const documentYaml = YAML::Load(core::serializeStationConfiguration(comparable));
		require(YAML::Dump(worldYaml["stationSystems"]) == YAML::Dump(documentYaml["stationSystems"]),
			"World and Station configuration writer disagreed");
		auto layoutless = documentYaml;
		layoutless.remove("layout");
		require(core::parseStationConfiguration(YAML::Dump(layoutless), restored, &diagnostic)
			&& restored.positions.empty(), "Layout-less document did not auto-layout");
		auto beforeInvalid = restored;
		auto invalidTransfer = bytes;
		auto invalidRate = invalidTransfer.find("ratePerSecond: 1");
		require(invalidRate != std::string::npos, "Expected transfer rate missing");
		invalidTransfer.replace(invalidRate, std::string("ratePerSecond: 1").size(), "ratePerSecond: -1");
		require(!core::parseStationConfiguration(invalidTransfer, restored, &diagnostic),
			"Broken transfer hid an invalid rate: " + diagnostic);
		require(restored.settings == beforeInvalid.settings,
			"Invalid transfer changed the previously loaded settings");
		auto invalid = bytes;
		auto at = invalid.find("capacity: 100");
		require(at != std::string::npos, "Expected capacity field missing");
		invalid.replace(at, std::string("capacity: 100").size(), "capacity: -1");
		auto before = restored;
		require(!core::parseStationConfiguration(invalid, restored, &diagnostic)
			&& restored.settings == before.settings && restored.positions == before.positions
			&& !diagnostic.empty(), "Invalid document partially replaced draft");
		// #775: an unresolved atmosphere must not hide invalid surviving
		// electrolyzer wiring at the standalone serialization/parsing seam.
		core::StationConfigurationDocument electrolysis;
		electrolysis.settings.enabled = true;
		electrolysis.settings.stores = {{"water", "kg", 100.0, 25.0},
			{"hydrogen", "kg", 100.0, 0.0}};
		core::ThermalReservoirSettings sink;
		sink.id = "sink"; sink.thermalClass = "moderate";
		sink.heatCapacityKJPerK = 1000.0;
		electrolysis.settings.thermalReservoirs = {sink};
		core::ElectrolyzerSettings cell;
		cell.id = "cell"; cell.waterStoreId = "water";
		cell.hydrogenStoreId = "hydrogen"; cell.atmosphereId = "missing";
		cell.heatSinkId = "sink"; cell.thermalClass = "moderate";
		electrolysis.settings.electrolyzers = {cell};
		core::StationConfigurationDocument parsedElectrolysis;
		require(core::parseStationConfiguration(core::serializeStationConfiguration(electrolysis),
			parsedElectrolysis, &diagnostic)
			&& parsedElectrolysis.settings == electrolysis.settings,
			"An otherwise valid electrolyzer draft with a missing atmosphere was refused: " + diagnostic);
		require(!core::validateStationSystemsSettings(electrolysis.settings, diagnostic),
			"Strict validation accepted an unresolved electrolyzer atmosphere");
		for (bool resolved : {false, true})
		{
			auto aliased = electrolysis;
			if (resolved)
			{
				core::AtmosphereSettings air;
				air.id = "missing"; air.volumeM3 = 100.0;
				air.thermalCapacityKJPerK = 1000.0;
				air.minPressureKPa = 1.0; air.maxPressureKPa = 200.0;
				aliased.settings.atmospheres = {air};
				auto const accepted = core::parseStationConfiguration(core::serializeStationConfiguration(aliased),
					parsedElectrolysis, &diagnostic);
				require(accepted, "Resolved electrolyzer control was refused: " + diagnostic);
				require(core::validateStationSystemsSettings(aliased.settings, diagnostic), diagnostic);
			}
			aliased.settings.electrolyzers[0].hydrogenStoreId = "water";
			auto const previous = parsedElectrolysis.settings;
			require(!core::parseStationConfiguration(core::serializeStationConfiguration(aliased),
				parsedElectrolysis, &diagnostic)
				&& diagnostic.find("feed water and a gas product") != std::string::npos,
				"Electrolyzer feed/product alias escaped draft validation (atmosphere resolved="
					+ std::to_string(resolved) + "): " + diagnostic);
			require(parsedElectrolysis.settings == previous,
				"Refused electrolyzer draft replaced the previously loaded settings");
		}
		core::setTransactionalWriteFailureAfterBytesForTesting(12);
		bool failed = false;
		try { core::saveStationConfiguration(path.string(), original); }
		catch (std::exception const&) { failed = true; }
		core::setTransactionalWriteFailureAfterBytesForTesting(0);
		require(failed && readFile(path) == bytes, "Failed save replaced previous file");
	}

	// #642: a file with broken references opens with those references
	// shown as broken, not refused. Draft-tolerant validation skips the
	// entry carrying an unresolvable reference while every other rule keeps
	// its full strictness: an invalid value anywhere else, or on the broken
	// entry's own fields checked before the reference, still refuses the
	// file. The broken entries survive in the draft byte-stably, and the
	// strict World path still refuses the same settings at Apply time.
	void brokenReferenceDocument(smoke::Context const& context)
	{
		auto const path = context.temporaryRoot() / "broken-references.station.yaml";
		core::StationConfigurationDocument draft;
		draft.settings.enabled = true;
		draft.settings.stores.push_back({"supply", "kg", 100.0, 50.0});
		// One broken reference per target kind: store, reservoir, coolant
		// loop, installation and atmosphere compartment.
		draft.settings.transfers.push_back({"pump", "supply", "ghostStore", "kg", 1.0, 1});
		draft.settings.conversions.push_back({"make",
			{{"ghostIn", 1.0}}, {{"supply", 0.5}}, 0});
		core::EquipmentLoadSettings loader;
		loader.id = "loader";
		loader.electricalDemandKW = 1.0;
		loader.thermalClass = "low";
		loader.materialExchanges.push_back({"ghostExchange", 1.0, true});
		draft.settings.equipmentLoads.push_back(loader);
		core::RadiatorSettings fin;
		fin.id = "fin";
		fin.reservoirId = "ghostReservoir";
		draft.settings.radiators.push_back(fin);
		core::ChillerSettings chiller;
		chiller.id = "chiller";
		chiller.thermalClass = "low";
		chiller.coldLoopId = "ghostLoop";
		draft.settings.chillers.push_back(chiller);
		core::GeneratorSettings generator;
		generator.id = "gen";
		generator.thermalClass = "low";
		generator.fuelStoreId = "ghostFuel";
		draft.settings.generators.push_back(generator);
		core::ResourceDeliverySettings delivery;
		delivery.id = "drop";
		delivery.storeId = "ghostDrop";
		delivery.unit = "kg";
		delivery.quantity = 5.0;
		delivery.firstOccurrenceSeconds = 1.0;
		draft.settings.deliveries.push_back(delivery);
		core::InstallationOutageSettings outage;
		outage.id = "down";
		outage.installationId = "ghostInstallation";
		outage.capacityFraction = 0.0;
		outage.firstOccurrenceSeconds = 1.0;
		outage.durationSeconds = 10.0;
		draft.settings.outages.push_back(outage);
		core::CrewSettings crew;
		crew.id = "crew";
		crew.atmosphereId = "ghostAtmosphere";
		draft.settings.crews.push_back(crew);
		core::CropPopulationSettings crop;
		crop.id = "crop";
		crop.atmosphereId = "ghostCompartment";
		draft.settings.cropPopulations.push_back(crop);
		core::WaterTreatmentSettings plant;
		plant.id = "plant";
		plant.recoveryRatio = 0.8;
		plant.sourceStoreId = "ghostSource";
		plant.treatedStoreId = "ghostTreated";
		plant.concentrateStoreId = "ghostConcentrate";
		draft.settings.waterTreatments.push_back(plant);
		draft.positions["Store:supply:0"] = {10.0f, 20.0f};

		auto const bytes = core::serializeStationConfiguration(draft);
		core::StationConfigurationDocument restored;
		std::string diagnostic;
		require(core::parseStationConfiguration(bytes, restored, &diagnostic),
			"Broken references refused the whole file: " + diagnostic);
		require(restored.settings == draft.settings && restored.positions == draft.positions,
			"Broken-reference load lost authored entries or layout");
		require(bytes == core::serializeStationConfiguration(restored),
			"Broken-reference document changed during round trip");
		// The strict World path still refuses the same settings: Apply
		// settings remains the gate where broken wiring cannot pass.
		std::string strictError;
		require(!core::validateStationSystemsSettings(restored.settings, strictError),
			"Strict validation wrongly accepted the broken-reference settings");

		// An invalid value in a non-broken entry still refuses the file.
		auto invalidElsewhere = draft;
		core::BatterySettings badBattery;
		badBattery.id = "bank";
		badBattery.nameplateKWh = 5.0;
		badBattery.usableKWh = 9.0;
		invalidElsewhere.settings.batteries.push_back(badBattery);
		require(!core::parseStationConfiguration(
			core::serializeStationConfiguration(invalidElsewhere), restored, &diagnostic),
			"An invalid value alongside broken references was accepted");
		require(restored.settings == draft.settings,
			"A refused broken-reference file disturbed the previously loaded draft");
		// A broken entry whose own field is checked before its reference is
		// still refused: the generator's thermal class is judged first.
		auto invalidOwnField = draft;
		invalidOwnField.settings.generators[0].thermalClass = "plasma";
		require(!core::parseStationConfiguration(
			core::serializeStationConfiguration(invalidOwnField), restored, &diagnostic),
			"An invalid own field on a broken-reference entry was accepted");
		require(restored.settings == draft.settings,
			"A refused own-field file disturbed the previously loaded draft");
		// The file seam keeps the same contract: a saved broken-reference
		// document loads through the real path unchanged.
		core::saveStationConfiguration(path.string(), draft);
		core::StationConfigurationDocument fromFile;
		require(core::loadStationConfiguration(path.string(), fromFile, &diagnostic)
			&& fromFile.settings == draft.settings, "Broken-reference file load failed: " + diagnostic);
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
		require(smoke::station::advanceSeconds(original, 2.0), "original scenario run failed");
		auto const expected = original.getStationSystemsSnapshot();
		auto const expectedEvents = original.getStationSystemsEvents();
		require(expected.worldTick == smoke::station::expectedStationSteps(original) && expectedEvents.size() == 5
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
			require(smoke::station::advanceSeconds(reopened, 2.0), "reopened scenario run failed");
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
		require(smoke::station::advanceSeconds(world, 1.0), "run before the failed save broke");
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
		require(smoke::station::advanceSeconds(world, 1.0)
			&& world.getStationSystemsSnapshot().completedTicks[0] == smoke::station::expectedStationSteps(world),
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
		replaceAll(legacy, "version: 106", "version: 64");
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
		replaceAll(staleStores, "version: 106", "version: 65");
		auto const staleStoresPath = directory / "stale-stores.world.yaml";
		writeFile(staleStoresPath, staleStores);
		try { (void)core::loadWorldDocument(staleStoresPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-66 document carrying resource stores was silently accepted");
		auto staleTransfers = withField;
		replaceAll(staleTransfers, "version: 106", "version: 66");
		auto const staleTransfersPath = directory / "stale-transfers.world.yaml";
		writeFile(staleTransfersPath, staleTransfers);
		refused = false;
		try { (void)core::loadWorldDocument(staleTransfersPath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-67 document carrying resource transfers was silently accepted");
		auto stale = withField;
		replaceAll(stale, "version: 106", "version: 64");
		auto const stalePath = directory / "stale.world.yaml";
		writeFile(stalePath, stale);
		refused = false;
		try { (void)core::loadWorldDocument(stalePath); }
		catch (std::exception const&) { refused = true; }
		require(refused, "a pre-65 document carrying Station data was silently accepted");

		core::World loadedWorld("Loaded", 2, 2);
		enable(loadedWorld, 2.0f);
		loadedWorld.resumeSimulation();
		require(smoke::station::advanceSeconds(loadedWorld, 0.5), "loaded World run failed");
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
		require(smoke::station::advanceSeconds(world, 0.5), "initial run failed");
		auto const live = world.getStationSystemsSnapshot();
		require(live.completedTicks[3] == smoke::station::expectedStationSteps(world), "station did not run before the edit");

		world.pauseSimulation();
		world.addRoom("Room B", 0, 0, 5, 4, 1);
		require(world.rebuildTraversalTopology(), "structural rebuild refused");
		require(world.getStationSystemsSnapshot() == live,
			"structural replay reset or changed the live station");
		require(world.getStationSystemsSettings().enabled,
			"structural replay lost the authored settings");

		world.resumeSimulation();
		require(smoke::station::advanceSeconds(world, 0.5), "run after the rebuild failed");
		require(world.getStationSystemsSnapshot().completedTicks[0] == smoke::station::expectedStationSteps(world),
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
		require(smoke::station::advanceSeconds(original, 1.0), "Compound original run failed");
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
			require(smoke::station::advanceSeconds(reopened, 1.0), "Compound reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"compound scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"compound event history is not reproducible from the document");
		}
		// A pre-68 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 67");
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
		require(smoke::station::advanceSeconds(original, 61.0), "Battery original run failed");
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
			require(smoke::station::advanceSeconds(reopened, 61.0), "Battery reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"battery scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"battery event history is not reproducible from the document");
		}
		// A pre-70 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 69");
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
		require(smoke::station::advanceSeconds(original, 61.0), "Power original run failed");
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
			require(smoke::station::advanceSeconds(reopened, 61.0), "Power reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"power scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"power event history is not reproducible from the document");
		}
		// A pre-69 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 68");
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
		require(smoke::station::advanceSeconds(original, 10.0), "Delivery original run failed");
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
			require(smoke::station::advanceSeconds(reopened, 10.0), "Delivery reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"delivery scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"delivery event history is not reproducible from the document");
		}
		// A pre-71 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 70");
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
		require(smoke::station::advanceSeconds(original, 10.0), "Ejection original run failed");
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
			require(smoke::station::advanceSeconds(reopened, 10.0), "Ejection reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"ejection scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"ejection event history is not reproducible from the document");
		}
		// Default omission preserves old documents; held-only is additive (#746).
		auto const withField = serializeYaml(original);
		require(withField.find("eligibility") == std::string::npos,
			"default eligibility changed existing document output");
		{
			auto legacy = withField;
			replaceAll(legacy, "version: 106", "version: 101");
			removeStepRatio(legacy);
			auto const path = directory / "pre-eligibility.world.yaml";
			writeFile(path, legacy);
			auto expectedLegacy = settings; expectedLegacy.systemsStepWorldTicks = 1;
			require(core::loadWorldDocument(path)->getStationSystemsSettings() == expectedLegacy,
				"old document did not retain default qualified-only semantics");
		}
		for (auto mode : {core::WasteEjectionEligibility::QualifiedAndHeld, core::WasteEjectionEligibility::HeldOnly})
		{
			settings.wasteEjections[0].eligibility = mode;
			original.pauseSimulation();
			require(original.configureStationSystems(settings, true, &diagnostic), diagnostic);
			for (auto const* suffix : {".world.yaml", ".world"})
			{
				auto const path = directory / (std::string(mode == core::WasteEjectionEligibility::HeldOnly
					? "held-only" : "emergency-drain") + suffix);
				original.saveTo(path.string());
				auto const reopened = core::loadWorldDocument(path);
				require(reopened->getStationSystemsSettings() == settings,
					"ejection eligibility did not round-trip");
				reopened->resumeSimulation();
				require(smoke::station::advanceSeconds(reopened, 10.0), "eligibility reopened run failed");
				auto const snap = reopened->getStationSystemsSnapshot();
				require(mode == core::WasteEjectionEligibility::HeldOnly
					? snap.stores[0].currentQualified == 12.0 && snap.stores[0].currentUnqualified == 0.0
						&& snap.wasteEjections[0].heldEjectedTotal == 5.0
					: snap.stores[0].currentQualified == 0.0 && snap.stores[0].currentUnqualified == 0.0
						&& snap.wasteEjections[0].qualifiedEjectedTotal == 12.0 && snap.wasteEjections[0].heldEjectedTotal == 5.0,
					"eligibility round-trip changed disposal semantics");
				core::World rerun("Ejection Round Trip", 2, 2);
				rerun.pauseSimulation(); rerun.finishBuild();
				require(rerun.configureStationSystems(settings, false, &diagnostic), diagnostic);
				rerun.resumeSimulation();
				require(smoke::station::advanceSeconds(rerun, 10.0), "eligibility original rerun failed");
				require(rerun.getStationSystemsSnapshot() == snap,
					"eligibility document changed the re-run snapshot");
			}
		}
		for (auto const* value : {"qualifiedOnly", "unknown", "false"})
		{
			auto document = serializeYaml(original);
			replaceAll(document, "eligibility: heldOnly", std::string("eligibility: ") + value);
			auto const path = directory / (std::string("eligibility-") + value + ".world.yaml");
			writeFile(path, document);
			bool accepted = false;
			try { accepted = core::loadWorldDocument(path)->getStationSystemsSettings().wasteEjections[0].eligibility
				== core::WasteEjectionEligibility::QualifiedOnly; }
			catch (std::exception const&) {}
			require(accepted == (std::string(value) == "qualifiedOnly"),
				"malformed/explicit default eligibility read incorrectly");
		}
		// A pre-82 document carrying the new fields is refused.
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 81");
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
	void containmentFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-containment";
		filesystem::create_directories(directory);
		core::ResourceStoreSettings hazRaw;
		hazRaw.id = "hazRaw";
		hazRaw.unit = "kg";
		hazRaw.capacity = 200.0;
		hazRaw.startingQualified = 100.0;
		core::ResourceStoreSettings hazTreated;
		hazTreated.id = "hazTreated";
		hazTreated.unit = "kg";
		hazTreated.capacity = 100.0;
		core::ResourceStoreSettings hazConcentrate;
		hazConcentrate.id = "hazConcentrate";
		hazConcentrate.unit = "kg";
		hazConcentrate.capacity = 100.0;
		core::ResourceStoreSettings hazConsumable;
		hazConsumable.id = "hazConsumable";
		hazConsumable.unit = "kg";
		hazConsumable.capacity = 100.0;
		hazConsumable.startingQualified = 10.0;
		core::ResourceStoreSettings potable;
		potable.id = "potable";
		potable.unit = "kg";
		potable.capacity = 100.0;
		potable.startingQualified = 50.0;
		core::WaterTreatmentSettings plant;
		plant.id = "plant";
		plant.sourceStoreId = "hazRaw";
		plant.treatedStoreId = "hazTreated";
		plant.concentrateStoreId = "hazConcentrate";
		plant.consumableStoreId = "hazConsumable";
		plant.throughputKgPerS = 0.005;
		plant.recoveryRatio = 0.8;
		plant.specificEnergyKWhPerKg = 0.5;
		plant.consumablePerKg = 0.01;
		core::World original("Containment Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 2.0f;
		settings.stores = { hazRaw, hazTreated, hazConcentrate, hazConsumable, potable };
		settings.waterTreatments = { plant };
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 20.0;
		settings.solarArrays = { field };
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Containment settings edit failed: " + diagnostic);
		original.resumeSimulation();
		require(smoke::station::advanceSeconds(original, 2.0), "Containment original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.waterTreatments.size() == 1
			&& expected.waterTreatments[0].qualityAccepted
			&& expected.waterTreatments[0].acceptedKg > 0.0,
			"Containment scenario produced the wrong observable state");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("containment") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("hazardClass") == std::string::npos
				&& document.find("clearanceStoreId") == std::string::npos
				&& document.find("clearancePerKg") == std::string::npos
				&& document.find("releaseToNormalClass") == std::string::npos,
				"document carries retired hazard-class or clearance fields");
			for (auto const* liveState : {"releaseCleared", "clearanceUsedTotal",
				"lastClearanceRequested", "lastClearanceUsed", "lastClearanceUnmet",
				"filtrationCompleted", "treatmentEvidenced", "heldKg", "acceptedKg"})
				require(document.find(liveState) == std::string::npos,
					std::string("live containment state leaked into the document: ") + liveState);
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"containment settings did not survive the file round trip");
			auto const fresh = reopened->getStationSystemsSnapshot();
			require(fresh.waterTreatments.size() == 1
				&& fresh.waterTreatments[0].acceptedKg == 0.0
				&& !fresh.waterTreatments[0].qualityAccepted
				&& fresh.stores[0].currentQualified == 100.0,
				"reopened World did not restart the authored containment scenario");
			reopened->resumeSimulation();
			require(smoke::station::advanceSeconds(reopened, 2.0), "Containment reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"containment scenario is not reproducible from the document");
		}
		// #776: exercise the authoritative reader directly so unrelated World
		// feature gates cannot mask retired-field diagnostics at older schemas.
		std::string preflightFailures;
		for (bool const microbial : {false, true})
		{
			core::StationSystemsSettings fixture;
			if (microbial) {
				core::MicrobialProcessorSettings processor;
				processor.id = "example";
				processor.waterQuality.maxTotalDissolved = 0.001;
				fixture.microbialProcessors = {processor};
			} else {
				auto treatment = plant;
				treatment.id = "example";
				fixture.waterTreatments = {treatment};
			}
			auto writer = core::YamlSerializer::toString();
			core::writeStationSystemsSettingsMap(*writer, fixture);
			writer->serialize();
			auto baseline = YAML::Load(writer->getSerializedString());
			baseline.remove("dispatchMode");
			baseline.remove("trendRetentionSeconds");
			baseline.remove("systemsStepWorldTicks");
			auto entry = baseline[microbial ? "microbialProcessors" : "waterTreatments"][0];
			entry.remove("removalFractions");
			auto read = [](YAML::Node const& document, std::uint32_t version) {
				auto input = core::YamlSerializer::fromString(YAML::Dump(document));
				input->deserialize();
				core::StationSystemsSettings parsed;
				core::readStationSystemsSettingsMap(*input, version, parsed, false);
				return parsed;
			};
			for (auto const version : {64u, 65u, 77u, 78u, 84u, 85u, 102u, 103u})
			{
				// Classless legacy entries retain their feature-version behavior.
				std::string baselineError;
				core::StationSystemsSettings parsed;
				try { parsed = read(baseline, version); }
				catch (std::exception const& error) { baselineError = error.what(); }
				auto const minimumVersion = microbial ? 85u : 78u;
				auto const expectedError = version < 65 ? "Station systems require World schema 65"
					: "require World schema " + std::to_string(minimumVersion);
				require(version < minimumVersion
					? baselineError.find(expectedError) != std::string::npos
					: baselineError.empty(), "classless entry load behavior changed: " + baselineError);
				if (microbial && version >= minimumVersion)
					require(parsed.microbialProcessors.size() == 1
						&& parsed.microbialProcessors[0].waterQuality == fixture.microbialProcessors[0].waterQuality,
						"legitimate microbial waterQuality gate was lost");
				for (auto const* field : {"hazardClass", "clearanceStoreId", "clearancePerKg", "releaseToNormalClass"})
				{
					if (microbial && std::string(field) != "hazardClass") continue;
					auto malformed = YAML::Clone(baseline);
					malformed[microbial ? "microbialProcessors" : "waterTreatments"][0][field] = "0";
					std::string message;
					try { read(malformed, version); }
					catch (std::exception const& error) { message = error.what(); }
					if (message.find("example") == std::string::npos
						|| message.find(field) == std::string::npos
						|| message.find("conversion required") == std::string::npos)
						preflightFailures += std::string(microbial ? "microbial " : "treatment ")
							+ field + " schema " + std::to_string(version) + ": " + message + "\n";
				}
			}
		}
		require(preflightFailures.empty(), "retired-field diagnostic precedence failures:\n" + preflightFailures);
		// A pre-88 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 87");
			auto const stalePath = directory / "stale-containment.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-88 document carrying containment fields was silently accepted");
		}
		// Retired fields are refused, including inert/default values and older schemas.
		for (auto const* field : {"clearanceStoreId: nowhere", "clearancePerKg: 0", "releaseToNormalClass: false"})
		for (auto const* version : {"103", "102"})
		{
			auto malformed = withField;
			replaceAll(malformed, "version: 106", std::string("version: ") + version);
			auto const at = malformed.find("sourceStoreId: hazRaw");
			auto const line = malformed.rfind('\n', at) + 1;
			malformed.insert(line, malformed.substr(line, at - line) + field + "\n");
			auto const path = directory / "retired-treatment-field.world.yaml";
			writeFile(path, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(path); }
			catch (std::exception const& error) {
				auto const message = std::string(error.what());
				refused = message.find(std::string(field).substr(0, std::string(field).find(':'))) != std::string::npos
					&& message.find("conversion required") != std::string::npos;
			}
			require(refused, std::string("retired treatment field did not produce a named conversion diagnostic: ") + field + " version " + version);
		}
		// The retired process hazardClass is refused with a named
		// conversion diagnostic at any schema version (#758).
		for (auto const* version : {"103", "90"})
		{
			auto retired = withField;
			replaceAll(retired, "version: 106", std::string("version: ") + version);
			auto const at = retired.find("sourceStoreId: hazRaw");
			require(at != std::string::npos, "water treatment anchor missing from the document");
			auto const lineStart = retired.rfind('\n', at) + 1;
			retired.insert(lineStart, retired.substr(lineStart, at - lineStart)
				+ "hazardClass: normal\n");
			auto const retiredPath = directory / ("retired-process-class-" + std::string(version) + ".world.yaml");
			writeFile(retiredPath, retired);
			bool refused = false;
			try { (void)core::loadWorldDocument(retiredPath); }
			catch (std::exception const& error) {
				auto const message = std::string(error.what());
				refused = message.find("hazardClass") != std::string::npos
					&& message.find("conversion required") != std::string::npos;
			}
			require(refused, std::string("the retired process hazardClass was accepted at version ") + version);
		}
		// The retired store hazardClass and the retired atmosphere
		// hazardClass (#756) are refused through the same shared guard,
		// each naming its entry and field at any schema version (#760).
		for (auto const* version : {"103", "90"})
		{
			auto retired = withField;
			replaceAll(retired, "version: 106", std::string("version: ") + version);
			auto const at = retired.find("id: potable");
			require(at != std::string::npos, "store anchor missing from the document");
			auto const lineStart = retired.rfind('\n', at) + 1;
			auto const lineEnd = retired.find('\n', at);
			// Match the entry's own field indentation rather than assuming
			// a width: the next authored field's column is the one truth.
			auto const unitAt = retired.find("unit: kg", at);
			auto const unitLineStart = retired.rfind('\n', unitAt) + 1;
			auto const fieldIndent = unitAt - unitLineStart;
			retired.insert(lineEnd + 1,
				std::string(fieldIndent, ' ') + "hazardClass: normal\n");
			auto const retiredPath = directory / ("retired-store-class-" + std::string(version) + ".world.yaml");
			writeFile(retiredPath, retired);
			bool refused = false;
			try { (void)core::loadWorldDocument(retiredPath); }
			catch (std::exception const& error) {
				auto const message = std::string(error.what());
				refused = message.find("hazardClass") != std::string::npos
					&& message.find("potable") != std::string::npos
					&& message.find("conversion required") != std::string::npos;
			}
			require(refused, std::string("the retired store hazardClass was accepted at version ") + version);
		}
		for (auto const* version : {"103", "90"})
		{
			auto retired = withField;
			replaceAll(retired, "version: 106", std::string("version: ") + version);
			require(retired.find("atmospheres:") == std::string::npos,
				"the containment fixture unexpectedly carries an atmospheres array");
			auto const at = retired.find("  waterTreatments:");
			require(at != std::string::npos, "water treatment array anchor missing from the document");
			retired.insert(at,
				"  atmospheres:\n"
				"    - id: habitat\n"
				"      hazardClass: normal\n");
			auto const retiredPath = directory / ("retired-atmosphere-class-" + std::string(version) + ".world.yaml");
			writeFile(retiredPath, retired);
			bool refused = false;
			try { (void)core::loadWorldDocument(retiredPath); }
			catch (std::exception const& error) {
				auto const message = std::string(error.what());
				refused = message.find("hazardClass") != std::string::npos
					&& message.find("habitat") != std::string::npos
					&& message.find("conversion required") != std::string::npos;
			}
			require(refused, std::string("the retired atmosphere hazardClass was accepted at version ") + version);
		}
		for (auto const* field : {"clearanceStoreId: nowhere", "clearancePerKg: 0", "releaseToNormalClass: false"})
		{
			core::StationConfigurationDocument document;
			document.settings = settings;
			auto text = core::serializeStationConfiguration(document);
			auto const at = text.find("sourceStoreId: hazRaw");
			auto const line = text.rfind('\n', at) + 1;
			text.insert(line, text.substr(line, at - line) + field + "\n");
			core::StationConfigurationDocument parsed;
			require(!core::parseStationConfiguration(text, parsed, &diagnostic)
				&& diagnostic.find(std::string(field).substr(0, std::string(field).find(':'))) != std::string::npos
				&& diagnostic.find("conversion required") != std::string::npos,
				"standalone configuration silently accepted a retired treatment field");
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
		// Explicitly recalibrate this gently warming replay fixture for
		// the corrected SI coefficient (#783); file areas are not migrated.
		panel.areaM2 = 5.0;
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
		require(smoke::station::advanceSeconds(original, 60.0), "Thermal original run failed");
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
			require(smoke::station::advanceSeconds(reopened, 60.0), "Thermal reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"thermal scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"thermal event history is not reproducible from the document");
		}
		// A pre-72 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 71");
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
		require(smoke::station::advanceSeconds(original, 60.0), "Buffer original run failed");
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
			require(smoke::station::advanceSeconds(reopened, 60.0), "Buffer reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"thermal buffer scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"thermal buffer event history is not reproducible from the document");
		}
		// A pre-81 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 80");
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
		require(smoke::station::advanceSeconds(original, 60.0), "Coolant original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.coolantLoops.size() == 2
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
				&& document.find("hazardClass") == std::string::npos
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
			require(smoke::station::advanceSeconds(reopened, 60.0), "Coolant reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"coolant scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"coolant event history is not reproducible from the document");
		}
		// A pre-73 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 72");
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
		// The retired coolant-loop class field is refused with a named
		// conversion diagnostic at any schema version (#757).
		for (auto const* version : {"103", "90"})
		{
			auto retired = withField;
			replaceAll(retired, "version: 106", std::string("version: ") + version);
			auto const at = retired.find("reservoirId: bus");
			require(at != std::string::npos, "coolant loop anchor missing from the document");
			auto const lineStart = retired.rfind('\n', at) + 1;
			retired.insert(lineStart, retired.substr(lineStart, at - lineStart)
				+ "hazardClass: normal\n");
			auto const retiredPath = directory / ("retired-coolant-class-" + std::string(version) + ".world.yaml");
			writeFile(retiredPath, retired);
			bool refused = false;
			try { (void)core::loadWorldDocument(retiredPath); }
			catch (std::exception const& error) {
				auto const message = std::string(error.what());
				refused = message.find("hazardClass") != std::string::npos
					&& message.find("conversion required") != std::string::npos;
			}
			require(refused, std::string("the retired coolant-loop hazardClass was accepted at version ") + version);
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
		require(smoke::station::advanceSeconds(original, 60.0), "Chiller original run failed");
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
			require(smoke::station::advanceSeconds(reopened, 60.0), "Chiller reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"chiller scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"chiller event history is not reproducible from the document");
		}
		// A pre-74 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 73");
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
		require(smoke::station::advanceSeconds(original, 60.0), "Generator original run failed");
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
			require(smoke::station::advanceSeconds(reopened, 60.0), "Generator reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"generator scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"generator event history is not reproducible from the document");
		}
		// A pre-75 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 74");
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

	// #413: scheduled installation outages persist as authored schedules
	// only. The window state, the started/completed occurrence counts and
	// each installation's effective availability are runtime state that is
	// reconstructed on load from the authored schedule and the World tick,
	// never written to the document.
	void outageFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-outages";
		filesystem::create_directories(directory);
		core::ResourceStoreSettings diesel;
		diesel.id = "diesel";
		diesel.unit = "kWh";
		diesel.capacity = 100000.0;
		diesel.startingQualified = 100000.0;
		diesel.protectedReserve = 0.0;
		core::ThermalReservoirSettings sink;
		sink.id = "hot";
		sink.thermalClass = "generation";
		sink.heatCapacityKJPerK = 100000.0;
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
		generator.maxRampKWPerSecond = 100.0;
		generator.commandedOutputKW = 10.0;
		generator.startupPowerKW = 1.0;
		generator.startupSeconds = 1.0;
		generator.shutdownHeatKW = 0.0;
		generator.shutdownSeconds = 0.0;
		generator.maxSinkTemperatureC = 90.0;
		core::ProfileSettings sun; sun.constantLevel = 1.0;
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 100.0;
		field.illumination = sun;
		core::InstallationOutageSettings recurring;
		recurring.id = "maint";
		recurring.installationId = "gen";
		recurring.capacityFraction = 0.4;
		recurring.firstOccurrenceSeconds = 10.0;
		recurring.durationSeconds = 5.0;
		recurring.recurrenceSeconds = 20.0;
		recurring.priority = 3;
		core::InstallationOutageSettings oneOff;
		oneOff.id = "once";
		oneOff.installationId = "gen";
		oneOff.capacityFraction = 0.0;
		oneOff.firstOccurrenceSeconds = 40.0;
		oneOff.durationSeconds = 6.0;
		oneOff.recurrenceSeconds = 0.0;
		oneOff.priority = 1;
		core::World original("Outage Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		settings.stores.push_back(diesel);
		settings.thermalReservoirs.push_back(sink);
		settings.generators.push_back(generator);
		settings.solarArrays.push_back(field);
		settings.outages.push_back(recurring);
		settings.outages.push_back(oneOff);
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Outage settings edit failed: " + diagnostic);
		original.resumeSimulation();
		require(smoke::station::advanceSeconds(original, 20.0), "Outage original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.outages.size() == 2
			&& expected.outages[0].occurrencesStarted >= 1,
			"Outage scenario produced the wrong observable state");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("outages") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("outages") != std::string::npos
				&& document.find("installationId") != std::string::npos
				&& document.find("capacityFraction") != std::string::npos
				&& document.find("firstOccurrenceSeconds") != std::string::npos
				&& document.find("durationSeconds") != std::string::npos
				&& document.find("recurrenceSeconds") != std::string::npos
				&& document.find("priority") != std::string::npos,
				"authored outage fields missing from the document");
			for (auto const* liveState : {"active", "occurrencesStarted",
				"occurrencesCompleted", "lastStartSeconds", "lastRestoreSeconds",
				"exhausted", "hasNextOccurrence", "nextOccurrenceSeconds",
				"installationAvailability"})
				require(document.find(liveState) == std::string::npos,
					std::string("live outage state leaked into the document: ") + liveState);
			require(document.find("maint") != std::string::npos
				&& document.find("once") != std::string::npos,
				"the authored outage ids are missing from the document");
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"outage settings did not survive the file round trip");
			require(reopened->getStationSystemsSnapshot().outages.size() == 2
				&& reopened->getStationSystemsSnapshot().outages[0].occurrencesStarted == 0
				&& reopened->getStationSystemsSnapshot().outages[0].active == false,
				"reopened World did not reconstruct the authored outage starting state");
			reopened->resumeSimulation();
			require(smoke::station::advanceSeconds(reopened, 20.0), "Outage reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"outage scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"outage event history is not reproducible from the document");
		}
		// A pre-90 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 89");
			auto const stalePath = directory / "stale-outages.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-90 document carrying outages was silently accepted");
		}
		// Malformed outage values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "capacityFraction: 0.4", "capacityFraction: 1.5");
			auto const malformedPath = directory / "malformed-outage-fraction.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "an outage capacity fraction above one was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "installationId: gen", "installationId: ghost");
			auto const malformedPath = directory / "malformed-outage-installation.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "an outage on an unknown installation was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "recurrenceSeconds: 20", "recurrenceSeconds: 2");
			auto const malformedPath = directory / "malformed-outage-recurrence.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a recurrence shorter than the outage window was accepted from a file");
		}
	}

	// #414: resource-constrained maintenance persists as the authored
	// schedule and requirement set only. The reservation state, phase,
	// progress, occurrence counts and spend are runtime state that is
	// reconstructed on load from the authored schedule and the World
	// tick, never written to the document. The reopened World replays
	// the closed scenario - same snapshots, same maintenance events -
	// and pre-94 documents or malformed requirement values are refused.
	void maintenanceFiles(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "station-maintenance";
		filesystem::create_directories(directory);
		core::ResourceStoreSettings diesel;
		diesel.id = "diesel";
		diesel.unit = "kWh";
		diesel.capacity = 100000.0;
		diesel.startingQualified = 100000.0;
		core::ResourceStoreSettings tools;
		tools.id = "tools";
		tools.unit = "kg";
		tools.capacity = 100.0;
		tools.startingQualified = 10.0;
		core::ResourceStoreSettings parts;
		parts.id = "parts";
		parts.unit = "kg";
		parts.capacity = 100.0;
		parts.startingQualified = 8.0;
		core::ResourceStoreSettings waste;
		waste.id = "waste";
		waste.unit = "kg";
		waste.capacity = 20.0;
		waste.startingQualified = 0.0;
		core::ThermalReservoirSettings sink;
		sink.id = "hot";
		sink.thermalClass = "generation";
		sink.heatCapacityKJPerK = 100000.0;
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
		generator.maxRampKWPerSecond = 100.0;
		generator.commandedOutputKW = 10.0;
		generator.startupPowerKW = 1.0;
		generator.startupSeconds = 1.0;
		generator.maxSinkTemperatureC = 90.0;
		core::ProfileSettings sun; sun.constantLevel = 1.0;
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 100.0;
		field.illumination = sun;
		core::MaintenanceJobSettings recurring;
		recurring.id = "service";
		recurring.installationId = "gen";
		recurring.firstOccurrenceSeconds = 10.0;
		recurring.durationSeconds = 8.0;
		recurring.recurrenceSeconds = 30.0;
		recurring.priority = 2;
		recurring.wasteStoreId = "waste";
		core::MaintenanceRequirementSettings toolRequirement;
		toolRequirement.storeId = "tools";
		toolRequirement.quantity = 3.0;
		toolRequirement.consumed = false;
		core::MaintenanceRequirementSettings partRequirement;
		partRequirement.storeId = "parts";
		partRequirement.quantity = 1.5;
		partRequirement.consumed = true;
		recurring.requirements.push_back(toolRequirement);
		recurring.requirements.push_back(partRequirement);
		core::MaintenanceJobSettings oneOff;
		oneOff.id = "overhaul";
		oneOff.installationId = "gen";
		oneOff.firstOccurrenceSeconds = 50.0;
		oneOff.durationSeconds = 6.0;
		oneOff.recurrenceSeconds = 0.0;
		oneOff.priority = 5;
		oneOff.wasteStoreId = "waste";
		oneOff.requirements.push_back(partRequirement);
		core::World original("Maintenance Round Trip", 2, 2);
		original.pauseSimulation();
		original.finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		settings.stores = {diesel, tools, parts, waste};
		settings.thermalReservoirs = {sink};
		settings.generators = {generator};
		settings.solarArrays = {field};
		settings.maintenanceJobs = {recurring, oneOff};
		std::string diagnostic;
		require(original.configureStationSystems(settings, false, &diagnostic),
			"Maintenance settings edit failed: " + diagnostic);
		original.resumeSimulation();
		require(smoke::station::advanceSeconds(original, 25.0), "Maintenance original run failed");
		auto const expected = original.getStationSystemsSnapshot();
		require(expected.maintenanceJobs.size() == 2
			&& expected.maintenanceJobs[0].occurrencesStarted >= 1,
			"Maintenance scenario produced the wrong observable state");
		for (auto const* suffix : {".world.yaml", ".world"})
		{
			auto const path = directory / (std::string("maintenance") + suffix);
			original.saveTo(path.string());
			auto const document = readFile(path);
			require(document.find("maintenanceJobs") != std::string::npos
				&& document.find("installationId") != std::string::npos
				&& document.find("firstOccurrenceSeconds") != std::string::npos
				&& document.find("durationSeconds") != std::string::npos
				&& document.find("recurrenceSeconds") != std::string::npos
				&& document.find("wasteStoreId") != std::string::npos
				&& document.find("requirements") != std::string::npos
				&& document.find("consumed") != std::string::npos,
				"authored maintenance fields missing from the document");
			for (auto const* liveState : {"progressSeconds", "occurrencesStarted",
				"occurrencesCompleted", "occurrencesMissed", "lastStartSeconds",
				"lastCompleteSeconds", "isolated", "blockReason", "reserved",
				"lastConsumed", "exhausted", "hasNextOccurrence", "phase"})
				require(document.find(liveState) == std::string::npos,
					std::string("live maintenance state leaked into the document: ") + liveState);
			require(document.find("service") != std::string::npos
				&& document.find("overhaul") != std::string::npos,
				"the authored maintenance ids are missing from the document");
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"maintenance settings did not survive the file round trip");
			require(reopened->getStationSystemsSnapshot().maintenanceJobs.size() == 2
				&& reopened->getStationSystemsSnapshot().maintenanceJobs[0].occurrencesStarted == 0
				&& reopened->getStationSystemsSnapshot().maintenanceJobs[0].phase
					== core::StationMaintenancePhase::Idle,
				"reopened World did not reconstruct the authored maintenance starting state");
			reopened->resumeSimulation();
			require(smoke::station::advanceSeconds(reopened, 25.0), "Maintenance reopened run failed");
			require(reopened->getStationSystemsSnapshot() == expected,
				"maintenance scenario is not reproducible from the document");
			require(reopened->getStationSystemsEvents() == original.getStationSystemsEvents(),
				"maintenance event history is not reproducible from the document");
		}
		// A pre-94 document carrying the new fields is refused.
		auto const withField = serializeYaml(original);
		{
			auto stale = withField;
			replaceAll(stale, "version: 106", "version: 93");
			auto const stalePath = directory / "stale-maintenance.world.yaml";
			writeFile(stalePath, stale);
			bool refused = false;
			try { (void)core::loadWorldDocument(stalePath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a pre-94 document carrying maintenance jobs was silently accepted");
		}
		// Malformed maintenance values in a file are rejected.
		{
			auto malformed = withField;
			replaceAll(malformed, "installationId: gen", "installationId: ghost");
			auto const malformedPath = directory / "malformed-maintenance-installation.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a maintenance job on an unknown installation was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "recurrenceSeconds: 30", "recurrenceSeconds: 4");
			auto const malformedPath = directory / "malformed-maintenance-recurrence.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a recurrence shorter than the maintenance window was accepted from a file");
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "wasteStoreId: waste", "wasteStoreId: nowhere");
			auto const malformedPath = directory / "malformed-maintenance-waste.world.yaml";
			writeFile(malformedPath, malformed);
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "a maintenance job on an unknown waste store was accepted from a file");
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
		require(smoke::station::advanceSeconds(original, 30.0), "Atmosphere original run failed");
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
		require(smoke::station::advanceSeconds(reopened, 30.0), "Atmosphere reopened run failed");
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
			replaceAll(stale, "version: 106", "version: 75");
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
		require(smoke::station::advanceSeconds(original, 30.0), "Crew original run failed");
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
		require(smoke::station::advanceSeconds(reopened, 30.0), "Crew reopened run failed");
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
			replaceAll(stale, "version: 106", "version: 76");
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
		// An aliased crew stream role (#600) is refused from a file with
		// the validation diagnostic: drinking water pointed at the food
		// store would spend one inventory for two substances the ledger
		// books separately, so the document never loads.
		{
			auto malformed = withField;
			replaceAll(malformed, "waterStoreId: water", "waterStoreId: food");
			auto const malformedPath = directory / "malformed-crew-aliased-water.world.yaml";
			writeFile(malformedPath, malformed);
			std::string message = "loaded without error";
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const& error) { refused = true; message = error.what(); }
			require(refused
				&& message.find("chemically distinct streams may not share one store")
					!= std::string::npos,
				"a crew aliasing its drinking water onto the food store was accepted from a file: " + message);
		}
		{
			auto malformed = withField;
			replaceAll(malformed, "organicWasteStoreId: organicWaste", "organicWasteStoreId: urine");
			auto const malformedPath = directory / "malformed-crew-aliased-waste.world.yaml";
			writeFile(malformedPath, malformed);
			std::string message = "loaded without error";
			bool refused = false;
			try { (void)core::loadWorldDocument(malformedPath); }
			catch (std::exception const& error) { refused = true; message = error.what(); }
			require(refused
				&& message.find("chemically distinct streams may not share one store")
					!= std::string::npos,
				"a crew aliasing its organic waste onto the urine store was accepted from a file: " + message);
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
	collector.atmosphereId = "habitat";
	collector.coolingLoopId = "chilled";
	collector.thermalClass = "moderate";
	collector.destStoreId = "raw";
	collector.maxRateKgPerS = 0.01;
	collector.targetRelativeHumidity = 0.5;
	core::WaterTreatmentSettings treatment;
	treatment.id = "plant";
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
	require(smoke::station::advanceSeconds(original, 30.0), "Water original run failed");
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
	require(smoke::station::advanceSeconds(reopened, 30.0), "Water reopened run failed");
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
		replaceAll(stale, "version: 106", "version: 77");
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
	require(smoke::station::advanceSeconds(original, 30.0), "Gas original run failed");
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
	require(smoke::station::advanceSeconds(reopened, 30.0), "Gas reopened run failed");
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
		replaceAll(stale, "version: 106", "version: 78");
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
	require(smoke::station::advanceSeconds(original, 30.0), "FuelCell original run failed");
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
	require(smoke::station::advanceSeconds(reopened, 30.0), "FuelCell reopened run failed");
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
		replaceAll(stale, "version: 106", "version: 79");
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
	require(smoke::station::advanceSeconds(original, 30.0), "Carbon original run failed");
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
	require(smoke::station::advanceSeconds(reopened, 30.0), "Carbon reopened run failed");
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
		replaceAll(stale, "version: 106", "version: 82");
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
	// An aliased stream role (#599) is refused from a file with the
	// validation diagnostic: the reactor's product water pointed at the
	// methane store would have spent one store's headroom twice on the
	// first tick, so the document never loads.
	{
		auto malformed = withField;
		replaceAll(malformed, "waterStoreId: rawWater", "waterStoreId: methane");
		auto const malformedPath = directory / "malformed-carbon-aliased-products.world.yaml";
		writeFile(malformedPath, malformed);
		std::string message = "loaded without error";
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const& error) { refused = true; message = error.what(); }
		require(refused
			&& message.find("chemically distinct streams may not share one store")
				!= std::string::npos,
			"a reactor aliasing its product water onto the methane store was accepted from a file: " + message);
	}
	{
		auto malformed = withField;
		replaceAll(malformed, "carbonStoreId: carbon", "carbonStoreId: h2");
		auto const malformedPath = directory / "malformed-carbon-aliased-processor.world.yaml";
		writeFile(malformedPath, malformed);
		std::string message = "loaded without error";
		bool refused = false;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const& error) { refused = true; message = error.what(); }
		require(refused
			&& message.find("chemically distinct streams may not share one store")
				!= std::string::npos,
			"a processor aliasing its solid carbon onto the hydrogen store was accepted from a file: " + message);
	}
}

void nutrientRecoveryFiles(smoke::Context const& context)
{
	namespace filesystem = std::filesystem;
	auto const directory = context.temporaryRoot() / "station-nutrient";
	filesystem::create_directories(directory);
	// Replace the first occurrence of `from` that appears after `anchor`
	// - blocks are serialised id-first, so anchoring on the id pins the
	// edit to the intended entry (#592).
	auto replaceOnceAfter = [](std::string& text, std::string const& anchor,
		std::string const& from, std::string const& to)
	{
		auto const at = text.find(anchor);
		require(at != std::string::npos, ("missing anchor: " + anchor).c_str());
		auto const hit = text.find(from, at);
		require(hit != std::string::npos, ("missing field after anchor: " + from).c_str());
		text.replace(hit, from.size(), to);
	};
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
	require(smoke::station::advanceSeconds(original, 30.0), "Nutrient original run failed");
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
	require(smoke::station::advanceSeconds(reopened, 30.0), "Nutrient reopened run failed");
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
		replaceAll(stale, "version: 106", "version: 84");
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

// A dedicated recovery chain runs through the document seam: the rig
// round-trips to both file formats and replays to the same snapshot, so
// the chain's stores, compartment and crop survive persistence intact.
void bioHazardFiles(smoke::Context const& context)
{
	namespace filesystem = std::filesystem;
	auto const directory = context.temporaryRoot() / "station-bio-hazard";
	filesystem::create_directories(directory);
	auto store = [](std::string const& id,
		double capacity, double starting)
	{
		core::ResourceStoreSettings settings;
		settings.id = id;
		settings.unit = "kg";
		settings.capacity = capacity;
		settings.startingQualified = starting;
		return settings;
	};
	auto compartment = [](std::string const& id)
	{
		core::AtmosphereSettings settings;
		settings.id = id;
		settings.volumeM3 = 100.0;
		settings.startingOxygenKg = 40.0;
		settings.startingNitrogenKg = 80.0;
		settings.startingCarbonDioxideKg = 5.0;
		settings.startingWaterVapourKg = 1.0;
		settings.startingTemperatureC = 22.0;
		settings.thermalCapacityKJPerK = 20000.0;
		settings.minPressureKPa = 1.0;
		settings.maxPressureKPa = 200.0;
		return settings;
	};
	core::ResourceCompositionSettings cropComposition;
	cropComposition.moisture = 0.0;
	cropComposition.nutrientNitrogen = 0.05;
	cropComposition.atmosphericNitrogen = 0.0;
	cropComposition.phosphorus = 0.02;
	cropComposition.potassium = 0.03;
	cropComposition.otherMinerals = 0.05;
	cropComposition.energyKJPerKgDry = 1700.0;
	core::CropPopulationSettings crop;
	crop.id = "canopy";
	crop.atmosphereId = "hazHabitat";
	crop.waterStoreId = "hazIrrigation";
	crop.nitrogenStoreId = "hazFertN";
	crop.phosphorusStoreId = "hazFertP";
	crop.potassiumStoreId = "hazFertK";
	crop.mineralsStoreId = "hazFertMin";
	crop.composition = cropComposition;
	crop.growingAreaM2 = 10.0;
	crop.maxBiomassDensityKgPerM2 = 2.0;
	crop.initialBiomassKg = 0.1;
	crop.lampRatedKW = 36.0;
	crop.illuminationProfile.constantLevel = 1.0;
	crop.circulationPowerKW = 0.1;
	crop.growthYieldKgPerKWh = 2.0;
	crop.respirationPerHour = 0.5;
	crop.transpirationKgPerKgGrowth = 1.0;
	crop.minGrowingTemperatureC = 10.0;
	crop.maxGrowingTemperatureC = 35.0;
	crop.foodStoreId = "hazFood";
	crop.wasteStoreId = "hazWaste";
	crop.edibleFraction = 0.6;
	crop.harvestTriggerKg = 0.3;
	crop.harvestFraction = 1.0;
	auto hazRig = [&]()
	{
		std::vector<core::ResourceStoreSettings> stores;
		stores.push_back(store("hazIrrigation", 200.0, 150.0));
		stores.push_back(store("hazFertN", 10.0, 5.0));
		stores.push_back(store("hazFertP", 10.0, 5.0));
		stores.push_back(store("hazFertK", 10.0, 5.0));
		stores.push_back(store("hazFertMin", 10.0, 5.0));
		stores.push_back(store("hazFood", 10.0, 0.0));
		stores.push_back(store("hazWaste", 10.0, 0.0));
		return stores;
	};
	core::World original("BioHazard Round Trip", 2, 2);
	original.pauseSimulation();
	original.finishBuild();
	core::StationSystemsSettings settings;
	settings.enabled = true;
	settings.observationIntervalSeconds = 2.0f;
	settings.stores = hazRig();
	settings.atmospheres = { compartment("hazHabitat") };
	settings.cropPopulations = { crop };
	core::SolarArraySettings field;
	field.id = "field";
	field.capacityKW = 100.0;
	settings.solarArrays = { field };
	std::string diagnostic;
	require(original.configureStationSystems(settings, false, &diagnostic),
		"BioHazard settings edit failed: " + diagnostic);
	original.resumeSimulation();
	require(smoke::station::advanceSeconds(original, 20.0), "BioHazard original run failed");
	auto const expected = original.getStationSystemsSnapshot();
	require(expected.cropPopulations.size() == 1
		&& expected.cropPopulations[0].harvestedKg > 0.0
		&& expected.cropPopulations[0].foodProducedKg > 0.0,
		"the hazardous crop scenario did not harvest before saving");
	for (auto const* suffix : {".world.yaml", ".world"})
	{
		auto const path = directory / (std::string("bio-hazard") + suffix);
		original.saveTo(path.string());
		auto const reopened = core::loadWorldDocument(path);
		require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
			"hazardous biological settings did not survive the file round trip");
		auto const fresh = reopened->getStationSystemsSnapshot();
		require(fresh.cropPopulations.size() == 1
			&& fresh.cropPopulations[0].harvestedKg == 0.0,
			"reopened World did not restart the hazardous biological scenario");
		reopened->resumeSimulation();
		require(smoke::station::advanceSeconds(reopened, 20.0), "BioHazard reopened run failed");
		require(reopened->getStationSystemsSnapshot() == expected,
			"the hazardous biological scenario is not reproducible from the document");
	}
}

// #581: the derived growing capacity - area times density for a crop,
// active culture volume times density per litre for an algae culture -
// must be finite. The two authored factors are each finite on their
// own, so only the product check closes the hole; the file seam
// enforces it: a document whose finite factors overflow the derived
// capacity to infinity is refused on load rather than publishing an
// infinite installation bound, while a large but finite product
// round-trips and replays intact.
void biologicalCapacityFiles(smoke::Context const& context)
{
	namespace filesystem = std::filesystem;
	auto const directory = context.temporaryRoot() / "station-bio-capacity";
	filesystem::create_directories(directory);
	auto replaceOnceAfter = [](std::string& text, std::string const& anchor,
		std::string const& from, std::string const& to)
	{
		auto const at = text.find(anchor);
		require(at != std::string::npos, ("missing anchor: " + anchor).c_str());
		auto const hit = text.find(from, at);
		require(hit != std::string::npos, ("missing field after anchor: " + from).c_str());
		text.replace(hit, from.size(), to);
	};
	auto store = [](std::string const& id, double capacity, double starting)
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
	habitat.volumeM3 = 100.0;
	habitat.startingOxygenKg = 40.0;
	habitat.startingNitrogenKg = 80.0;
	habitat.startingCarbonDioxideKg = 5.0;
	habitat.startingWaterVapourKg = 1.0;
	habitat.startingTemperatureC = 22.0;
	habitat.thermalCapacityKJPerK = 20000.0;
	habitat.minPressureKPa = 1.0;
	habitat.maxPressureKPa = 200.0;
	core::ResourceCompositionSettings dryComposition;
	dryComposition.moisture = 0.0;
	dryComposition.nutrientNitrogen = 0.05;
	dryComposition.atmosphericNitrogen = 0.0;
	dryComposition.phosphorus = 0.02;
	dryComposition.potassium = 0.03;
	dryComposition.otherMinerals = 0.05;
	dryComposition.energyKJPerKgDry = 1700.0;
	core::CropPopulationSettings crop;
	crop.id = "canopy";
	crop.atmosphereId = "habitat";
	crop.waterStoreId = "irrigation";
	crop.nitrogenStoreId = "fertN";
	crop.phosphorusStoreId = "fertP";
	crop.potassiumStoreId = "fertK";
	crop.mineralsStoreId = "fertMin";
	crop.composition = dryComposition;
	crop.growingAreaM2 = 10.0;
	crop.maxBiomassDensityKgPerM2 = 2.0;
	crop.initialBiomassKg = 1.0;
	crop.lampRatedKW = 3.6;
	crop.illuminationProfile.constantLevel = 1.0;
	crop.circulationPowerKW = 0.1;
	crop.growthYieldKgPerKWh = 2.0;
	crop.respirationPerHour = 0.5;
	crop.transpirationKgPerKgGrowth = 1.0;
	crop.minGrowingTemperatureC = 10.0;
	crop.maxGrowingTemperatureC = 35.0;
	core::AlgaeCultureSettings algae;
	algae.id = "pond";
	algae.atmosphereId = "habitat";
	algae.waterStoreId = "makeup";
	algae.nitrogenStoreId = "cN";
	algae.phosphorusStoreId = "cP";
	algae.potassiumStoreId = "cK";
	algae.mineralsStoreId = "cMin";
	algae.composition = dryComposition;
	algae.cultureVolumeL = 1000.0;
	algae.maxBiomassDensityKgPerL = 0.002;
	algae.initialBiomassKg = 0.1;
	algae.lampRatedKW = 36.0;
	algae.illuminationProfile.constantLevel = 1.0;
	algae.circulationPowerKW = 0.1;
	algae.growthYieldKgPerKWh = 2.0;
	algae.respirationPerHour = 0.5;
	algae.evaporationKgPerKgGrowth = 1.0;
	algae.minGrowingTemperatureC = 10.0;
	algae.maxGrowingTemperatureC = 35.0;
	algae.foodFraction = 0.4;
	algae.recoveryFraction = 0.3;
	algae.harvestTriggerKg = 0.0;
	algae.harvestFraction = 1.0;
	core::World original("BioCapacity Round Trip", 2, 2);
	original.pauseSimulation();
	original.finishBuild();
	core::StationSystemsSettings settings;
	settings.enabled = true;
	settings.observationIntervalSeconds = 1.0f;
	settings.stores = { store("irrigation", 200.0, 150.0), store("fertN", 10.0, 5.0),
		store("fertP", 10.0, 5.0), store("fertK", 10.0, 5.0), store("fertMin", 10.0, 5.0),
		store("makeup", 200.0, 150.0), store("cN", 10.0, 5.0), store("cP", 10.0, 5.0),
		store("cK", 10.0, 5.0), store("cMin", 10.0, 5.0) };
	settings.atmospheres = { habitat };
	settings.cropPopulations = { crop };
	settings.algaeCultures = { algae };
	std::string diagnostic;
	require(original.configureStationSystems(settings, false, &diagnostic),
		"BioCapacity settings edit failed: " + diagnostic);
	auto const withField = serializeYaml(original);
	// A crop document whose finite area and finite density overflow the
	// derived Growing capacity to infinity is refused on load.
	{
		auto malformed = withField;
		replaceOnceAfter(malformed, "id: canopy",
			"growingAreaM2: 10", "growingAreaM2: 1.0e+308");
		replaceOnceAfter(malformed, "id: canopy",
			"maxBiomassDensityKgPerM2: 2", "maxBiomassDensityKgPerM2: 1.0e+308");
		auto const malformedPath = directory / "crop-capacity-overflow.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		std::string loadError;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const& error) { refused = true; loadError = error.what(); }
		require(refused
			&& loadError.find("overflows to a non-finite magnitude") != std::string::npos,
			"a crop document whose area times density overflows the Growing capacity was accepted from a file");
	}
	// An algae document whose finite volume and finite density per litre
	// overflow the derived culture capacity to infinity is refused on
	// load.
	{
		auto malformed = withField;
		replaceOnceAfter(malformed, "id: pond",
			"cultureVolumeL: 1000", "cultureVolumeL: 1.0e+308");
		replaceOnceAfter(malformed, "id: pond",
			"maxBiomassDensityKgPerL: 0.002", "maxBiomassDensityKgPerL: 1.0e+308");
		auto const malformedPath = directory / "algae-capacity-overflow.world.yaml";
		writeFile(malformedPath, malformed);
		bool refused = false;
		std::string loadError;
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const& error) { refused = true; loadError = error.what(); }
		require(refused
			&& loadError.find("overflows to a non-finite magnitude") != std::string::npos,
			"an algae document whose volume times density overflows the culture capacity was accepted from a file");
	}
	// A large but finite product - 1e154 times 1e154 is 1e308, finite -
	// stays valid: the document loads, carries the authored factors, and
	// the reopened World runs healthy ticks that publish the finite
	// derived capacity.
	{
		auto repaired = withField;
		replaceOnceAfter(repaired, "id: canopy",
			"growingAreaM2: 10", "growingAreaM2: 1.0e+154");
		replaceOnceAfter(repaired, "id: canopy",
			"maxBiomassDensityKgPerM2: 2", "maxBiomassDensityKgPerM2: 1.0e+154");
		replaceOnceAfter(repaired, "id: pond",
			"cultureVolumeL: 1000", "cultureVolumeL: 1.0e+154");
		replaceOnceAfter(repaired, "id: pond",
			"maxBiomassDensityKgPerL: 0.002", "maxBiomassDensityKgPerL: 1.0e+154");
		auto const validPath = directory / "bio-capacity-huge-finite.world.yaml";
		writeFile(validPath, repaired);
		auto const reopened = core::loadWorldDocument(validPath);
		require(reopened->getStationSystemsSettings().cropPopulations[0].growingAreaM2 == 1e154
			&& reopened->getStationSystemsSettings().cropPopulations[0].maxBiomassDensityKgPerM2 == 1e154
			&& reopened->getStationSystemsSettings().algaeCultures[0].cultureVolumeL == 1e154
			&& reopened->getStationSystemsSettings().algaeCultures[0].maxBiomassDensityKgPerL == 1e154,
			"the large but finite biological capacity factors did not survive the file round trip");
		reopened->resumeSimulation();
		require(smoke::station::advanceSeconds(reopened, 2.0),
			"the reopened huge-but-finite biological rig failed to run");
		auto const snap = reopened->getStationSystemsSnapshot();
		require(snap.healthy
			&& snap.cropPopulations[0].capacityKg == 1e308
			&& snap.algaeCultures[0].capacityKg == 1e308,
			"the reopened huge-but-finite derived capacities did not publish intact");
	}
}

// #647: the power and conversion chain survives the headless file
// round trip. Every machine type is authored with its references wired,
// saved through both the YAML and binary World writers, and reloaded:
// the settings come back byte-equal and the authored link table - the
// identity of every reference the Flow view draws - enumerates the
// same set on both sides.
void powerChainFiles(smoke::Context const& context)
{
	namespace filesystem = std::filesystem;
	auto const directory = context.temporaryRoot() / "station-power-chain";
	filesystem::create_directories(directory);
	auto store = [](std::string const& id, std::string const& unit)
	{
		core::ResourceStoreSettings entry;
		entry.id = id; entry.unit = unit; entry.capacity = 1000.0;
		entry.startingQualified = 500.0;
		return entry;
	};
	auto reservoir = [](std::string const& id, std::string const& thermalClass)
	{
		core::ThermalReservoirSettings entry;
		entry.id = id; entry.thermalClass = thermalClass;
		entry.heatCapacityKJPerK = 1000.0;
		return entry;
	};
	core::StationSystemsSettings settings;
	settings.enabled = true;
	settings.observationIntervalSeconds = 1.0f;
	settings.stores = {store("water", "kg"), store("diesel", "kJ"),
		store("co2", "kg"), store("h2", "kg"), store("o2", "kg"),
		store("ch4", "kg"), store("rawwater", "kg"), store("carbon", "kg")};
	settings.thermalReservoirs = {reservoir("warmSink", "moderate"),
		reservoir("genSink", "generation"), reservoir("coldSink", "low")};
	core::EquipmentLoadSettings load;
	load.id = "load1"; load.thermalClass = "moderate";
	load.electricalDemandKW = 2.0; load.heatOutputKW = 1.0;
	load.heatSinkId = "warmSink";
	load.materialExchanges.push_back({"water", 1.0, true});
	load.materialExchanges.push_back({"rawwater", 1.0, false});
	settings.equipmentLoads = {load};
	core::BatterySettings battery;
	battery.id = "bat1"; battery.nameplateKWh = 10.0; battery.usableKWh = 8.0;
	battery.startingStoredKWh = 8.0; battery.maxChargeKW = 2.0;
	battery.maxDischargeKW = 4.0;
	settings.batteries = {battery};
	core::GeneratorSettings generator;
	generator.id = "gen1"; generator.thermalClass = "generation";
	generator.fuelStoreId = "diesel"; generator.heatSinkId = "genSink";
	generator.ratedOutputKW = 10.0; generator.efficiency = 0.4;
	generator.maxRampKWPerSecond = 2.0;
	settings.generators = {generator};
	core::FuelCellSettings cell;
	cell.id = "cell1"; cell.thermalClass = "low";
	cell.ratedOutputKW = 10.0; cell.efficiency = 0.6;
	cell.hydrogenStoreId = "h2"; cell.oxygenStoreId = "o2";
	cell.waterStoreId = "rawwater"; cell.heatSinkId = "coldSink";
	settings.fuelCells = {cell};
	core::SabatierReactorSettings reactor;
	reactor.id = "sab1"; reactor.thermalClass = "low";
	reactor.rateKgPerS = 0.001;
	reactor.co2StoreId = "co2"; reactor.hydrogenStoreId = "h2";
	reactor.methaneStoreId = "ch4"; reactor.waterStoreId = "rawwater";
	reactor.heatSinkId = "coldSink";
	settings.sabatierReactors = {reactor};
	core::MethaneProcessorSettings processor;
	processor.id = "mp1";	processor.thermalClass = "low"; processor.rateKgPerS = 0.001;
	processor.specificEnergyKWhPerKgCH4 = 2.0;
	processor.methaneStoreId = "ch4"; processor.hydrogenStoreId = "h2";
	processor.carbonStoreId = "carbon"; processor.heatSinkId = "coldSink";
	settings.methaneProcessors = {processor};
	// Rebuild every reference through the same link seam as canvas drags,
	// rather than only testing serialization of pre-filled fields.
	auto const plannedLinks = core::stationAuthoredLinks(settings);
	for (auto const& authored : plannedLinks)
		require(core::clearStationLink(settings, authored.identity), "could not clear a chain reference");
	require(core::stationAuthoredLinks(settings).empty(), "the new chain retained authored references");
	for (auto const& authored : plannedLinks)
	{
		core::CandidateLink const candidate{authored.from, authored.to};
		auto const verdict = core::evaluateStationLink(settings, candidate);
		require(verdict.allowed, ("building the chain refused a legal link: " + verdict.reason).c_str());
		require(core::applyStationLink(settings, candidate), "building the chain failed to write a reference");
	}
	core::World original("Power Chain Round Trip", 2, 2);
	original.pauseSimulation(); original.finishBuild();
	std::string diagnostic;
	require(original.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
	original.markSaved();
	// The authored link table is the Flow view's view of the references;
	// capture it before the round trip so the reloaded one can be
	// compared as a set of identities.
	auto const beforeLinks = core::stationAuthoredLinks(original.getStationSystemsSettings());
	require(beforeLinks.size() >= 14,
		("the wired chain enumerated too few references: "
			+ std::to_string(beforeLinks.size())).c_str());
	for (auto const* suffix : {".world.yaml", ".world"})
	{
		auto const path = directory / (std::string("power-chain") + suffix);
		original.saveTo(path.string());
		auto const reopened = core::loadWorldDocument(path);
		require(reopened != nullptr, "power chain document failed to load");
		require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
			("power chain settings did not survive the " + std::string(suffix) + " round trip").c_str());
		auto const afterLinks = core::stationAuthoredLinks(reopened->getStationSystemsSettings());
		auto const identities = [](std::vector<core::AuthoredLink> const& links)
		{
			std::vector<std::string> out;
			for (auto const& entry : links) out.push_back(entry.identity);
			std::sort(out.begin(), out.end());
			return out;
		};
		require(identities(afterLinks) == identities(beforeLinks),
			("the authored link table changed across the " + std::string(suffix) + " round trip").c_str());
		// Every machine's own reference fields came back populated.
		auto const& back = reopened->getStationSystemsSettings();
		require(back.equipmentLoads.front().heatSinkId == "warmSink"
			&& back.generators.front().fuelStoreId == "diesel"
			&& back.fuelCells.front().oxygenStoreId == "o2"
			&& back.sabatierReactors.front().methaneStoreId == "ch4"
			&& back.methaneProcessors.front().carbonStoreId == "carbon",
			("a machine reference was lost across the " + std::string(suffix) + " round trip").c_str());
	}
}

void registerStationDocumentChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"stepRatioConfiguration", stepRatioConfiguration});
	checks.push_back({"materialIdentity", materialIdentity});
	checks.push_back({"configurationFile", configurationFile});
	checks.push_back({"brokenReferenceDocument", brokenReferenceDocument});
	checks.push_back({"fileRoundTrip", fileRoundTrip});
	checks.push_back({"writeFailure", writeFailure});
	checks.push_back({"legacyFiles", legacyAndMalformedFiles});
	checks.push_back({"structuralReplay", structuralReplay});
	checks.push_back({"compoundFiles", compoundFiles});
	checks.push_back({"powerFiles", powerFiles});
	checks.push_back({"powerChainFiles", powerChainFiles});
	checks.push_back({"batteryFiles", batteryFiles});
	checks.push_back({"deliveryFiles", deliveryFiles});
	checks.push_back({"wasteEjectionFiles", wasteEjectionFiles});
	checks.push_back({"thermalFiles", thermalFiles});
	checks.push_back({"bufferFiles", bufferFiles});
	checks.push_back({"coolantFiles", coolantFiles});
	checks.push_back({"chillerFiles", chillerFiles});
	checks.push_back({"generatorFiles", generatorFiles});
	checks.push_back({"outageFiles", outageFiles});
	checks.push_back({"maintenanceFiles", maintenanceFiles});
	checks.push_back({"atmosphereFiles", atmosphereFiles});
	checks.push_back({"crewFiles", crewFiles});
	checks.push_back({"bioHazardFiles", bioHazardFiles});
	checks.push_back({"bioCapacityFiles", biologicalCapacityFiles});
	checks.push_back({"waterFiles", waterFiles});
	checks.push_back({"containmentFiles", containmentFiles});
	checks.push_back({"gasFiles", gasFiles});
	checks.push_back({"fuelCellFiles", fuelCellFiles});
	checks.push_back({"carbonRecoveryFiles", carbonRecoveryFiles});
	checks.push_back({"nutrientRecoveryFiles", nutrientRecoveryFiles});
}
