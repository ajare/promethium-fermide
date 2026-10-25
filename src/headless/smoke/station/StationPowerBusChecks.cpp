#include "StationRig.h"
#include "Smoke.h"
#include "core/StationPowerBuses.h"
#include "core/StationConfigurationDocument.h"
#include "core/BinarySerializer.h"
#include "core/WorldDocument.h"
#include <yaml-cpp/yaml.h>

namespace
{
	using namespace smoke::station;
	core::StationSystemsSettings fixture()
	{
		core::StationSystemsSettings s;
		s.enabled = true;
		core::SolarArraySettings solar;
		solar.id = "solar"; solar.capacityKW = 20;
		solar.illumination.constantLevel = 1;
		s.solarArrays.push_back(solar);
		core::EquipmentLoadSettings load;
		load.id = "load"; load.electricalDemandKW = 8; load.thermalClass = "moderate";
		load.operatingProfile.constantLevel = 1;
		s.equipmentLoads.push_back(load);
		return s;
	}
	std::string settingsYaml(core::StationSystemsSettings const& settings)
	{
		auto out = core::YamlSerializer::toString();
		out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, settings);
		out->endMap(); out->serialize(); return out->getSerializedString();
	}
	void readSettings(std::string const& text, uint32_t version,
		core::StationSystemsSettings& target, bool validate = true)
	{
		auto in = core::YamlSerializer::fromString(text); in->deserialize();
		in->beginMap("root"); core::readStationSystemsSettingsMap(*in, version, target, validate);
		in->endMap();
	}
	void powerBusMigration(smoke::Context const&)
	{
		auto const before = fixture();
		auto const bytes = settingsYaml(before);
		require(bytes.find("powerBuses") == std::string::npos, "default-only settings changed historical wire shape");
		for (uint32_t version : {111u, 122u, 123u, 124u})
		{
			core::StationSystemsSettings migrated;
			migrated.powerBuses = {{"stale", "Must be replaced", 1, 2}};
			readSettings(bytes, version, migrated);
			require(migrated == before, "legacy document did not migrate to exactly the default main bus");
			core::StationSystemsRuntime baseline(before, 1.0f), restored(migrated, 1.0f);
			require(baseline.advanceTo(10) && restored.advanceTo(10), "migration scenario drive failed");
			auto const expected = baseline.snapshot(), actual = restored.snapshot();
			require(expected.power == actual.power, "migration changed dispatch behaviour");
			require(actual.power.buses.size() == 1 && actual.power.buses[0].busId == "main",
				"snapshot does not expose exactly one main bus");
			auto const& bus = actual.power.buses[0];
			require(bus.ratedCapacityKW == 0 && !bus.tripped && bus.loadFraction == 0,
				"migrated bus is not unrated/inert");
			require(bus.generationKW == actual.power.poolGenerationKW
				&& bus.suppliedKW == actual.power.totalSuppliedKW && bus.suppliedKW == 8
				&& bus.demandKW == actual.power.dispatchDemandKW && bus.unmetKW == 0
				&& bus.surplusKW == actual.power.surplusKW,
				"single-bus wrapper disagrees with preserved station totals");
		}
		auto dark = before;
		dark.solarArrays[0].illumination.constantLevel = 0;
		dark.equipmentLoads[0].maxCurtailment = 0;
		core::StationSystemsRuntime starved(dark, 1.0f);
		require(starved.advanceTo(10), "unmet migration scenario failed");
		auto const unmet = starved.snapshot().power;
		require(unmet.totalUnmetKW == 8 && unmet.buses[0].demandKW == 8
			&& unmet.buses[0].unmetKW == 8 && unmet.buses[0].suppliedKW == 0,
			"single-bus wrapper hid refused/Shed load demand");
		// The packaged station exercises generation, storage, protected services
		// and internal consumers at the same production migration seam.
		auto const reference = core::bundledStationReference().settings;
		core::StationSystemsSettings migrated;
		readSettings(settingsYaml(reference), 123, migrated);
		core::StationSystemsRuntime original(reference, 1.0f), reopened(migrated, 1.0f);
		require(original.advanceTo(10) && reopened.advanceTo(10), "reference migration drive failed");
		require(original.snapshot().power == reopened.snapshot().power,
			"default-main migration changed the reference station electrical account");
	}
	void powerBusPersistence(smoke::Context const&)
	{
		auto settings = fixture();
		settings.powerBuses = {{"main", "Primary", 12, 4}, {"backup", "Emergency", 0, 0}};
		settings.solarArrays[0].busIds = {"main", "backup"};
		settings.equipmentLoads[0].supplyBusIds = {"main", "backup"};
		settings.equipmentLoads[0].supplyMode = core::StationSupplyMode::Transfer;
		settings.equipmentLoads[0].transferReturnDwellSeconds = 17;
		core::StationConfigurationDocument doc; doc.settings = settings;
		auto const bytes = core::serializeStationConfiguration(doc);
		require(bytes.find("schemaVersion: 124") != std::string::npos, "bus topology did not select schema 124");
		std::string diagnostic;
		core::StationConfigurationDocument loaded;
		auto const parsed = core::parseStationConfiguration(bytes, loaded, &diagnostic);
		require(parsed, diagnostic.c_str());
		require(loaded.settings == settings && core::serializeStationConfiguration(loaded) == bytes,
			"native bus settings did not round trip canonically");
		for (uint32_t old : {122u, 123u})
		{
			auto target = fixture(); auto const unchanged = target;
			bool refused = false;
			try { readSettings(settingsYaml(settings), old, target); } catch (std::exception const&) { refused = true; }
			require(refused && target == unchanged, "old schema silently accepted buses or partially mutated settings");
		}
		core::World world("Bus persistence", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto const configured = world.configureStationSystems(settings, false, &diagnostic);
		require(configured, diagnostic.c_str());
		for (bool binary : {false, true})
		{
			core::SerializationWorkData work;
			auto serialize = [&](auto out) {
				world.serialize(*out, work); out->serialize(); return out->getSerializedString();
			};
			auto const text = binary ? serialize(core::BinarySerializer::toString()) : serialize(core::YamlSerializer::toString());
			std::unique_ptr<core::Serializer> in;
			if (binary) in = core::BinarySerializer::fromString(text);
			else in = core::YamlSerializer::fromString(text);
			in->deserialize();
			core::World restored("Restored", 2, 2);
			require(restored.deserialize(*in, work), "World bus round trip failed");
			require(restored.getStationSystemsSettings() == settings, "World lost bus settings or membership");
		}
	}
	void powerBusRoles(smoke::Context const&)
	{
		// Every electrical settings type participates, including all internal
		// consumers and generator startup. Their unrelated wiring is outside
		// this codec check; validate=false is the prefab/draft reader seam.
		core::StationSystemsSettings s;
		s.solarArrays.resize(1); s.generators.resize(1); s.fuelCells.resize(1);
		s.heatEngines.resize(1); s.batteries.resize(1); s.equipmentLoads.resize(1);
		s.coolantLoops.resize(1); s.chillers.resize(1); s.waterTreatments.resize(1);
		s.electrolyzers.resize(1); s.co2Captures.resize(1); s.sabatierReactors.resize(1);
		s.methaneProcessors.resize(1); s.microbialProcessors.resize(1);
		s.cropPopulations.resize(1); s.algaeCultures.resize(1);
		core::forEachStationPowerMember(s, [&](auto& entry) {
			if constexpr (requires { entry.busIds; })
			{
				require(entry.busIds == std::vector<std::string>{"main"}, "source default is not main");
				entry.busIds = {"secondary", "main"};
			}
			if constexpr (requires { entry.supplyBusIds; })
			{
				require(entry.supplyBusIds == std::vector<std::string>{"main"}
					&& entry.supplyMode == core::StationSupplyMode::Single, "consumer default is not Single/main");
				entry.supplyBusIds = {"main", "secondary"};
				entry.supplyMode = core::StationSupplyMode::Shared;
				entry.transferReturnDwellSeconds = 42;
			}
		});
		s.powerBuses.push_back({"secondary", "Secondary", 50, 5});
		core::StationSystemsSettings restored;
		readSettings(settingsYaml(s), 124, restored, false);
		require(restored == s, "an electrical role lost its authored bus fields");
		core::forEachStationPowerMember(s, [&](auto& entry) {
			if constexpr (requires { entry.busIds; }) entry.busIds = {"main"};
			if constexpr (requires { entry.supplyBusIds; })
			{
				entry.supplyBusIds = {"main"}; entry.supplyMode = core::StationSupplyMode::Single;
				entry.transferReturnDwellSeconds = 30;
			}
		});
		s.powerBuses = {core::PowerBusSettings{}};
		readSettings(settingsYaml(s), 123, restored, false);
		require(restored == s, "an electrical role failed legacy main-bus migration");
	}
	void powerBusValidation(smoke::Context const&)
	{
		auto const good = fixture(); std::string diagnostic;
		core::World world("Bus validation", 2, 2); world.pauseSimulation(); world.finishBuild();
		auto const configured = world.configureStationSystems(good, false, &diagnostic);
		require(configured, diagnostic.c_str());
		auto refuse = [&](core::StationSystemsSettings const& bad) {
			require(!world.configureStationSystems(bad, false, &diagnostic) && !diagnostic.empty()
				&& world.getStationSystemsSettings() == good, "bad bus authoring mutated World or lacked diagnostic");
			auto target = good;
			bool rejected = false;
			try { readSettings(settingsYaml(bad), 124, target); } catch (std::exception const&) { rejected = true; }
			require(rejected && target == good, "invalid bus read partially changed caller settings");
		};
		auto bad = good; bad.powerBuses.push_back(bad.powerBuses[0]); refuse(bad);
		bad = good; bad.powerBuses[0].id.clear(); refuse(bad);
		bad = good; bad.powerBuses.clear(); refuse(bad);
		for (double value : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
		{
			bad = good; bad.powerBuses[0].ratedCapacityKW = value; refuse(bad);
			bad = good; bad.powerBuses[0].tripPersistenceSeconds = value; refuse(bad);
			bad = good; bad.equipmentLoads[0].transferReturnDwellSeconds = value; refuse(bad);
		}
		bad = good; bad.equipmentLoads[0].transferReturnDwellSeconds = 5;
		auto malformed = YAML::Load(settingsYaml(bad));
		malformed["stationSystems"]["equipmentLoads"][0]["supplyMode"] = "typo";
		auto target = good; bool refused = false;
		try { readSettings(YAML::Dump(malformed), 124, target); } catch (std::exception const&) { refused = true; }
		require(refused && target == good, "unknown supply mode was accepted or partially applied");
	}
}
void registerStationPowerBusChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"powerBusMigration", powerBusMigration});
	checks.push_back({"powerBusPersistence", powerBusPersistence});
	checks.push_back({"powerBusRoles", powerBusRoles});
	checks.push_back({"powerBusValidation", powerBusValidation});
}
