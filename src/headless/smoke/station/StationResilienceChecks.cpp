#include "StationResilienceChecks.h"
#include "StationRig.h"
#include "core/AgentTagRegistryDocument.h"

namespace
{
	using namespace smoke::station;

void dispatch(smoke::Context const&)
{
	// #415: the dispatcher closes the loop the operator used to close by
	// hand. In automatic mode the station commands its own generators and
	// fuel-cell backups against the real demand signal, honouring startup,
	// ramp, fuel-above-reserve and cooling limits, so an eclipse is bridged
	// by the battery and then carried by started generation while the
	// declared reserves are never spent to meet ordinary load. Manual mode
	// keeps the pre-#415 contract: the operator's setpoints are the only
	// commands and the dispatcher never moves a source.
	auto const approx = [](double actual, double expected, double tolerance = 1e-6)
	{ return std::abs(actual - expected) <= tolerance; };
	auto const replaceAll = [](std::string text, std::string const& from, std::string const& to)
	{
		for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
			text.replace(pos, from.size(), to);
		return text;
	};
	auto sawEvent = [](core::World& world, std::string const& needle)
	{
		for (auto const& event : world.getStationSystemsEvents())
			if (event.message.find(needle) != std::string::npos) return true;
		return false;
	};
	auto sinkReservoir = [](double capacityKJPerK, double startingC)
	{
		core::ThermalReservoirSettings settings;
		settings.id = "hot";
		settings.thermalClass = "generation";
		settings.heatCapacityKJPerK = capacityKJPerK;
		settings.startingTemperatureC = startingC;
		settings.minTemperatureC = -50.0;
		settings.maxTemperatureC = 200.0;
		return settings;
	};
	auto fuelStore = [](std::string id, std::string unit, double capacity,
		double starting, double reserve)
	{
		core::ResourceStoreSettings settings;
		settings.id = std::move(id);
		settings.unit = std::move(unit);
		settings.capacity = capacity;
		settings.startingQualified = starting;
		settings.protectedReserve = reserve;
		return settings;
	};
	auto generatorSettings = [](double ratedKW)
	{
		core::GeneratorSettings settings;
		settings.id = "gen";
		settings.fuelStoreId = "diesel";
		settings.thermalClass = "generation";
		settings.heatSinkId = "hot";
		settings.ratedOutputKW = ratedKW;
		settings.efficiency = 0.5;
		settings.maxRampKWPerSecond = 5.0;
		settings.commandedOutputKW = 0.0;
		settings.startupPowerKW = 1.0;
		settings.startupSeconds = 6.0;
		settings.shutdownHeatKW = 1.0;
		settings.shutdownSeconds = 10.0;
		settings.maxSinkTemperatureC = 90.0;
		return settings;
	};
	auto battery = [](double nameplateKWh, double usableKWh, double startingKWh,
		double maxDischargeKW)
	{
		core::BatterySettings settings;
		settings.id = "bank";
		settings.nameplateKWh = nameplateKWh;
		settings.usableKWh = usableKWh;
		settings.startingStoredKWh = startingKWh;
		settings.maxChargeKW = 10.0;
		settings.maxDischargeKW = maxDischargeKW;
		settings.chargeEfficiency = 1.0;
		settings.dischargeEfficiency = 1.0;
		return settings;
	};
	auto criticalLoad = [](double demandKW)
	{
		core::EquipmentLoadSettings settings;
		settings.id = "critical";
		settings.electricalDemandKW = demandKW;
		settings.operatingProfile = constantProfile(1.0);
		settings.heatOutputKW = 0.0;
		settings.thermalClass = "low";
		settings.coolingDisposition = core::StationCoolingDisposition::Exported;
		settings.criticalFloorKW = demandKW;
		settings.priority = 100;
		settings.maxCurtailment = 0.0;
		return settings;
	};
	auto solarAt = [](double fraction)
	{
		core::SolarArraySettings settings;
		settings.id = "array";
		settings.capacityKW = 12.0;
		settings.illumination = constantProfile(fraction);
		return settings;
	};
	struct Rig
	{
		std::vector<core::ResourceStoreSettings> stores;
		std::vector<core::ThermalReservoirSettings> reservoirs;
		std::vector<core::GeneratorSettings> generators;
		std::vector<core::BatterySettings> batteries;
		std::vector<core::SolarArraySettings> solar;
		std::vector<core::EquipmentLoadSettings> loads;
		std::vector<core::FuelCellSettings> cells;
		core::StationDispatchMode mode{core::StationDispatchMode::Manual};
	};
	auto run = [](core::World& world, Rig rig, int ticks)
	{
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		settings.dispatchMode = rig.mode;
		settings.stores = std::move(rig.stores);
		settings.thermalReservoirs = std::move(rig.reservoirs);
		settings.generators = std::move(rig.generators);
		settings.batteries = std::move(rig.batteries);
		settings.solarArrays = std::move(rig.solar);
		settings.equipmentLoads = std::move(rig.loads);
		settings.fuelCells = std::move(rig.cells);
		std::string diagnostic;
		require(world.configureStationSystems(settings, false, &diagnostic),
			("automatic dispatch rig refused: " + diagnostic).c_str());
		world.resumeSimulation();
		require(world.advanceTicks(ticks), "automatic dispatch run failed");
	};
	auto fullRig = [&](core::StationDispatchMode mode, double diesel, double dieselReserve)
	{
		Rig rig;
		rig.mode = mode;
		rig.stores = {fuelStore("diesel", "kWh", 1000.0, diesel, dieselReserve),
			{"water", "kg", 1000.0, 500.0, 0.0}};
		rig.reservoirs = {sinkReservoir(100000.0, 20.0)};
		rig.generators = {generatorSettings(10.0)};
		rig.batteries = {battery(12.0, 10.0, 12.0, 10.0)};
		rig.loads = {criticalLoad(6.0)};
		return rig;
	};
	auto makeWorld = [](char const* name)
	{
		return std::make_unique<core::World>(name, 2, 2);
	};

	// Manual mode keeps the operator in charge: with the generator set to
	// zero and the sun gone, the battery alone carries the critical load
	// and drains; the dispatcher never starts the generator. 100 s of
	// eclipse at 6 kW drains ~1/6 kWh from the bank.
	{
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		Rig rig = fullRig(core::StationDispatchMode::Manual, 1000.0, 0.0);
		rig.solar = {solarAt(0.0)};
		run(world, std::move(rig), 6000);
		auto const& power = world.getStationSystemsSnapshot().power;
		require(power.dispatchMode == core::StationDispatchMode::Manual,
			"manual mode not reported in the snapshot");
		require(approx(power.generators[0].outputKW, 0.0),
			"manual mode let the dispatcher start the generator");
		require(power.batteries[0].storedKWh < 11.9,
			"manual eclipse did not drain the battery carrying the load");
		require(approx(power.totalUnmetKW, 0.0, 1e-9),
			"manual mode left the critical load unmet while the battery could carry it");
		require(!power.reserveExhausted,
			"manual mode raised the automatic reserve alarm");
	}

	// Automatic mode bridges the eclipse with the battery and then starts
	// the generator against the deficit; once the generator carries the
	// load the battery stops draining and holds far above the manual drain.
	{
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		Rig rig = fullRig(core::StationDispatchMode::Automatic, 1000.0, 0.0);
		rig.solar = {solarAt(0.0)};
		run(world, std::move(rig), 6000);
		auto const& power = world.getStationSystemsSnapshot().power;
		require(power.dispatchMode == core::StationDispatchMode::Automatic,
			"automatic mode not reported in the snapshot");
		require(power.generators[0].dispatchCommandKW > 0.0,
			"automatic mode issued no dispatch command to the generator");
		require(power.generators[0].outputKW > 5.9,
			"automatic generator did not ramp up to carry the load");
		require(approx(power.totalUnmetKW, 0.0, 1e-9),
			"automatic dispatch left the critical load unmet once the generator was online");
		// The battery bridged startup and ramp, then held: it is well above
		// the manual-mode drain and only slightly below its starting charge.
		require(power.batteries[0].storedKWh > 11.9
			&& power.batteries[0].storedKWh < 12.0,
			"automatic battery did not bridge and then hold the load");
		require(approx(power.batteryDischargeKW, 0.0, 1e-9),
			"automatic battery still discharging once the generator carried the load");
		require(sawEvent(world, "Dispatch brought generator"),
			"automatic generator start was not recorded as a dispatch action");
		require(!power.reserveExhausted,
			"reserve alarm raised while the generator could still carry the load");
	}

	// Automatic mode with the drawable fuel exhausted: the dispatcher
	// cannot draw the protected reserve for ordinary load, so the generator
	// never runs, the battery rides the load down to its own protected
	// reserve floor, and the reserve-exhausted alarm is raised rather
	// than the protected fuel reserve being silently spent.
	{
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		Rig rig = fullRig(core::StationDispatchMode::Automatic, 5.0, 5.0);
		// A small bank power-capped to carry the load: 0.5 kWh cyclable
		// above its 2 kWh reserve floor. The load is fully curtable so it
		// keeps drawing as the bank's deliverable tapers, letting the bank
		// actually reach its protected floor rather than strand a residual.
		rig.batteries = {battery(2.5, 0.5, 2.5, 10.0)};
		rig.loads[0].maxCurtailment = 1.0;
		rig.solar = {solarAt(0.0)};
		run(world, std::move(rig), 24000);
		auto const& power = world.getStationSystemsSnapshot().power;
		require(power.reserveExhausted,
			"running the drawable fuel dry did not raise the reserve alarm");
		require(power.totalUnmetKW > 1e-6,
			"reserve exhaustion left no unmet demand while the load still needed power");
		require(sawEvent(world, "Electrical reserve exhausted"),
			"reserve exhaustion was not recorded as an alarm");
		auto const& diesel = world.getStationSystemsSnapshot().stores[0];
		require(diesel.currentQualified + diesel.currentUnqualified >= 5.0 - 1e-9,
			"automatic dispatch spent the protected fuel reserve");
	}

	// Automatic mode with the generator out of fuel: the fuel-cell backup
	// engages so the critical load is still carried when the primary
	// source cannot run.
	{
		Rig rig;
		rig.mode = core::StationDispatchMode::Automatic;
		rig.stores = {fuelStore("diesel", "kWh", 1000.0, 0.0, 0.0),
			fuelStore("h2", "kg", 100.0, 100.0, 0.0),
			fuelStore("o2", "kg", 800.0, 800.0, 0.0),
			{"water", "kg", 1000.0, 0.0, 0.0}};
		rig.reservoirs = {sinkReservoir(100000.0, 20.0)};
		rig.generators = {generatorSettings(10.0)};
		rig.batteries = {battery(12.0, 10.0, 12.0, 10.0)};
		rig.loads = {criticalLoad(4.0)};
		core::FuelCellSettings cell;
		cell.id = "cell";
		cell.hydrogenStoreId = "h2";
		cell.oxygenStoreId = "o2";
		cell.waterStoreId = "water";
		cell.thermalClass = "generation";
		cell.heatSinkId = "hot";
		cell.ratedOutputKW = 6.0;
		cell.efficiency = 0.6;
		cell.commandedOutputKW = 0.0;
		cell.maxSinkTemperatureC = 80.0;
		rig.cells = {cell};
		rig.solar = {solarAt(0.0)};
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		run(world, std::move(rig), 120);
		auto const& power = world.getStationSystemsSnapshot().power;
		require(power.backupEngaged,
			"fuel-cell backup did not engage with the generator out of fuel");
		require(world.getStationSystemsSnapshot().fuelCells[0].lastOutputKW > 1e-6,
			"fuel-cell backup engaged but produced no power");
		require(sawEvent(world, "Backup generation engaged"),
			"backup engagement was not recorded as an alarm");
	}

	// #578: no-battery bootstrapping. A stopped generator that needs
	// cranking no longer reserves the dispatch target it cannot serve:
	// the residual load reaches the fuel-cell backup, and the backup
	// funds the legitimate start. With no battery and no sun the old
	// controller let the generator's phantom reservation starve the
	// backup: the critical load stayed unmet forever while dispatch
	// reported zero shortfall. Here the backup carries the load and
	// the crank, the generator starts, ramps, and takes the load back.
	{
		auto bootstrapRig = [&]
		{
			Rig rig;
			rig.mode = core::StationDispatchMode::Automatic;
			rig.stores = {fuelStore("diesel", "kWh", 1000.0, 100.0, 0.0),
				fuelStore("h2", "kg", 100.0, 100.0, 0.0),
				fuelStore("o2", "kg", 800.0, 800.0, 0.0),
				{"water", "kg", 1000.0, 0.0, 0.0}};
			rig.reservoirs = {sinkReservoir(100000.0, 20.0)};
			rig.generators = {generatorSettings(10.0)};
			rig.loads = {criticalLoad(6.0)};
			core::FuelCellSettings cell;
			cell.id = "cell";
			cell.hydrogenStoreId = "h2";
			cell.oxygenStoreId = "o2";
			cell.waterStoreId = "water";
			cell.thermalClass = "generation";
			cell.heatSinkId = "hot";
			cell.ratedOutputKW = 10.0;
			cell.efficiency = 0.6;
			cell.commandedOutputKW = 0.0;
			cell.maxSinkTemperatureC = 150.0;
			rig.cells = {cell};
			return rig;
		};
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		run(world, bootstrapRig(), 120);
		{
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& power = snap.power;
			require(power.generators[0].phase == core::StationGeneratorPhase::Starting,
				"the backup-funded start never began without a battery");
			require(power.generators[0].dispatchCommandKW > 0.0,
				"the dispatcher never commanded the stopped generator to start");
			require(power.generators[0].outputKW <= 1e-12,
				"a cranking generator reported output it cannot deliver");
			require(power.generatorStartupDrawKW > 0.9 && power.generatorStartupDrawKW < 1.1,
				"the crank did not draw its startup power from the backup-funded pool");
			require(snap.fuelCells[0].lastOutputKW > 6.9
				&& snap.fuelCells[0].lastOutputKW < 7.1,
				"the backup did not carry the load plus the startup support");
			require(approx(power.dispatchCommittedKW, power.dispatchTargetKW, 1e-9),
				"the bootstrap committed did not match the funded target");
			require(approx(power.dispatchShortfallKW, 0.0, 1e-9),
				"the backup-funded bootstrap reported a shortfall");
			require(approx(power.unmetCriticalKW, 0.0, 1e-9),
				"the critical load went unmet while the backup could carry it");
		}
		// Past startup and the ramp: the generator takes the load back
		// and the backup stands down.
		require(world.advanceTicks(600), "bootstrap run failed after startup");
		{
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& power = snap.power;
			require(power.generators[0].phase == core::StationGeneratorPhase::Running,
				"the backup-funded generator never reached Running");
			require(power.generators[0].outputKW > 5.9,
				"the generator did not ramp up to carry the load");
			require(snap.fuelCells[0].lastOutputKW < 1e-9,
				"the backup kept burning after the generator took the load");
			require(approx(power.dispatchShortfallKW, 0.0, 1e-9),
				"steady-state bootstrap reported a shortfall");
			require(approx(power.unmetCriticalKW, 0.0, 1e-9),
				"the load went unmet once the generator carried it");
		}
		// Determinism: the bootstrap path adds no run-to-run variance.
		{
			auto const probe = [&](Rig rig)
			{
				core::World world("Dispatch", 2, 2);
				world.pauseSimulation();
				world.finishBuild();
				run(world, std::move(rig), 500);
				auto const& snap = world.getStationSystemsSnapshot();
				auto const& power = snap.power;
				return std::array{power.generators[0].outputKW,
					snap.fuelCells[0].lastOutputKW,
					power.dispatchCommittedKW, power.dispatchShortfallKW};
			};
			require(probe(bootstrapRig()) == probe(bootstrapRig()),
				"the no-battery bootstrap was not deterministic");
		}
	}

	// #578: with no backup and nothing to crank on, the stopped
	// generator's unavailable capacity is never reported as a covered
	// target: the shortfall stays explicit beside the unmet load. The
	// old controller booked the full command as committed and reported
	// zero shortfall while the machine delivered nothing.
	{
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		Rig rig = fullRig(core::StationDispatchMode::Automatic, 1000.0, 0.0);
		rig.batteries.clear();
		rig.solar = {solarAt(0.0)};
		run(world, std::move(rig), 120);
		auto const& power = world.getStationSystemsSnapshot().power;
		require(power.generators[0].phase == core::StationGeneratorPhase::Stopped,
			"a generator with no cranking source somehow left Stopped");
		require(approx(power.dispatchCommittedKW, 0.0, 1e-9),
			"unavailable generator capacity was booked as committed");
		require(power.dispatchShortfallKW > 1e-9,
			"the dispatch shortfall hid the capacity no source could serve");
		require(power.unmetCriticalKW > 1e-9,
			"the unmet critical load vanished from the account");
	}

	// #578: a slow ramp never books as capacity before it arrives.
	// The backup carries the difference tick by tick while the
	// generator climbs its ramp, and the committed account tracks what
	// the sources actually deliver, not what the generator will reach.
	{
		Rig rig;
		rig.mode = core::StationDispatchMode::Automatic;
		rig.stores = {fuelStore("diesel", "kWh", 1000.0, 100.0, 0.0),
			fuelStore("h2", "kg", 100.0, 100.0, 0.0),
			fuelStore("o2", "kg", 800.0, 800.0, 0.0),
			{"water", "kg", 1000.0, 0.0, 0.0}};
		rig.reservoirs = {sinkReservoir(100000.0, 20.0)};
		auto slow = generatorSettings(10.0);
		slow.maxRampKWPerSecond = 0.1;
		rig.generators = {slow};
		rig.loads = {criticalLoad(6.0)};
		core::FuelCellSettings cell;
		cell.id = "cell";
		cell.hydrogenStoreId = "h2";
		cell.oxygenStoreId = "o2";
		cell.waterStoreId = "water";
		cell.thermalClass = "generation";
		cell.heatSinkId = "hot";
		cell.ratedOutputKW = 10.0;
		cell.efficiency = 0.6;
		cell.commandedOutputKW = 0.0;
		cell.maxSinkTemperatureC = 150.0;
		rig.cells = {cell};
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		// 1560 ticks at 60 Hz is 26 s: 6 s of crank plus 20 s of
		// 0.1 kW/s ramp, so the generator sits near 2 kW of its 10 kW
		// rating while the backup carries the rest.
		run(world, std::move(rig), 1560);
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& power = snap.power;
		require(power.generators[0].phase == core::StationGeneratorPhase::Running,
			"the slow-ramp generator never reached Running");
		require(power.generators[0].outputKW > 1.5 && power.generators[0].outputKW < 2.5,
			"the slow ramp did not hold to its ramp-limited output");
		require(approx(power.generators[0].outputKW + snap.fuelCells[0].lastOutputKW, 6.0, 1e-6),
			"the backup did not carry exactly the ramp-limited difference");
		require(approx(power.dispatchCommittedKW, 6.0, 1e-6),
			"the committed account did not match what the sources deliver");
		require(approx(power.dispatchShortfallKW, 0.0, 1e-9),
			"the slow-ramp run reported a shortfall the backup could cover");
		require(approx(power.unmetCriticalKW, 0.0, 1e-9),
			"the load went unmet while the backup covered the ramp gap");
	}

	// #578: a source trip during startup. The sink starts just below
	// the generator's maximum and the cranking heat itself pushes the
	// cooling out of qualification mid-crank: the start fails whole,
	// the generator stays stopped, and the backup keeps carrying the
	// load on its own sink with no unmet demand and no phantom
	// committed capacity.
	{
		Rig rig;
		rig.mode = core::StationDispatchMode::Automatic;
		rig.stores = {fuelStore("diesel", "kWh", 1000.0, 100.0, 0.0),
			fuelStore("h2", "kg", 100.0, 100.0, 0.0),
			fuelStore("o2", "kg", 800.0, 800.0, 0.0),
			{"water", "kg", 1000.0, 0.0, 0.0}};
		core::ThermalReservoirSettings hot;
		hot.id = "hot";
		hot.thermalClass = "generation";
		hot.heatCapacityKJPerK = 10.0;
		hot.startingTemperatureC = 89.9;
		hot.minTemperatureC = -50.0;
		hot.maxTemperatureC = 200.0;
		core::ThermalReservoirSettings fcSink;
		fcSink.id = "fcSink";
		fcSink.thermalClass = "generation";
		fcSink.heatCapacityKJPerK = 100000.0;
		fcSink.startingTemperatureC = 20.0;
		fcSink.minTemperatureC = -50.0;
		fcSink.maxTemperatureC = 200.0;
		rig.reservoirs = {hot, fcSink};
		rig.generators = {generatorSettings(10.0)};
		rig.loads = {criticalLoad(6.0)};
		core::FuelCellSettings cell;
		cell.id = "cell";
		cell.hydrogenStoreId = "h2";
		cell.oxygenStoreId = "o2";
		cell.waterStoreId = "water";
		cell.thermalClass = "generation";
		cell.heatSinkId = "fcSink";
		cell.ratedOutputKW = 10.0;
		cell.efficiency = 0.6;
		cell.commandedOutputKW = 0.0;
		cell.maxSinkTemperatureC = 150.0;
		rig.cells = {cell};
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		// The 1 kW crank into a 10 kJ/K sink warms it 0.1 K/s: the
		// 0.1 K headroom is gone within a second and the start fails
		// mid-crank before the 6 s of startup energy can complete.
		run(world, std::move(rig), 2);
		bool sawMidCrankTrip = false;
		for (int tick = 0; tick < 238; ++tick)
		{
			require(world.advanceTicks(1), "trip run failed");
			auto const& p = world.getStationSystemsSnapshot().power;
			if (p.generators[0].blockReason.find("cooling lost during startup")
				!= std::string::npos)
				sawMidCrankTrip = true;
		}
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& power = snap.power;
		require(sawMidCrankTrip,
			"the mid-crank cooling trip was never reported as the block reason");
		require(power.generators[0].phase == core::StationGeneratorPhase::Stopped,
			"a generator whose cooling failed mid-crank did not fall back to Stopped");
		require(approx(power.generators[0].outputKW, 0.0, 1e-12),
			"a trip mid-crank reported generator output");
		require(snap.fuelCells[0].lastOutputKW > 5.9 && snap.fuelCells[0].lastOutputKW < 6.1,
			"the backup did not carry the load alone after the start failed");
		require(approx(power.dispatchShortfallKW, 0.0, 1e-9),
			"the failed-start run reported a shortfall the backup covers");
		require(approx(power.unmetCriticalKW, 0.0, 1e-9),
			"the load went unmet after the start failed while the backup could carry it");
	}

	// The dispatch mode persists and round-trips; a document without the
	// dispatch mode field is refused rather than silently defaulting.
	{
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		Rig rig = fullRig(core::StationDispatchMode::Automatic, 1000.0, 0.0);
		rig.solar = {solarAt(0.5)};
		run(world, std::move(rig), 10);
		auto const text = yaml(world);
		require(text.find("dispatchMode: automatic") != std::string::npos,
			"automatic dispatch mode was not saved");
		core::SerializationWorkData data;
		auto reader = core::YamlSerializer::fromString(text);
		reader->deserialize();
		core::World loaded("Loaded", 2, 2);
		loaded.deserialize(*reader, data);
		require(loaded.getStationSystemsSnapshot().power.dispatchMode
			== core::StationDispatchMode::Automatic,
			"automatic dispatch mode did not survive the round trip");
		auto stale = text;
		require(stale.find("version: 103") != std::string::npos,
			"the fixture document carries no version line");
		require(stale.find("dispatchMode: automatic") != std::string::npos,
			"the fixture document lost the dispatch mode before the guard test");
		// A pre-91 document that still carries the dispatch mode is refused
		// rather than silently dropping the controller setting.
		stale = replaceAll(stale, "version: 103", "version: 90");
		bool refused = false;
		try
		{
			auto staleReader = core::YamlSerializer::fromString(stale);
			staleReader->deserialize();
			core::World staleWorld("Stale", 2, 2);
			staleWorld.deserialize(*staleReader, data);
		}
		catch (std::exception const&)
		{
			refused = true;
		}
		require(refused, "a pre-91 document carrying the dispatch mode was silently accepted");
	}

	// Determinism: the same rig advanced the same way produces the same
	// dispatch outputs, so the controller adds no run-to-run variance.
	{
		auto const first = [&]
		{
			core::World world("Dispatch", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			Rig rig = fullRig(core::StationDispatchMode::Automatic, 1000.0, 0.0);
			rig.solar = {solarAt(0.0)};
			run(world, std::move(rig), 40);
			return world.getStationSystemsSnapshot().power.generators[0].outputKW;
		}();
		auto const second = [&]
		{
			core::World world("Dispatch", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			Rig rig = fullRig(core::StationDispatchMode::Automatic, 1000.0, 0.0);
			rig.solar = {solarAt(0.0)};
			run(world, std::move(rig), 40);
			return world.getStationSystemsSnapshot().power.generators[0].outputKW;
		}();
		require(approx(first, second), "automatic dispatch was not deterministic");
	}
}

// #416: protected cross-system service dependencies. Containment
// circulation, biological essential circulation and authored critical
// cooling are Critical station services: they are admitted from the
// actual generation-and-battery pool ahead of every discretionary
// external load, so shedding experiments or lighting never cuts the
// support services protected loads depend on. What the pool cannot
// deliver stays explicit unmet critical minimum with its reason;
// generator shutdown-cooling stays visible even when the machine's
// electrical output has stopped. Everything runs through the World
// tick/snapshot/event seam with the bundled Lua sandbox auditing the
// same rules inside each private instance.
void protectedServices(smoke::Context const&)
{
	auto const closeTo = [](double actual, double expected, double tolerance = 1e-6)
	{ return std::abs(actual - expected) <= tolerance; };
	auto const sawEvent = [](core::World& world, std::string const& needle)
	{
		for (auto const& event : world.getStationSystemsEvents())
			if (event.message.find(needle) != std::string::npos) return true;
		return false;
	};
	auto const constantProfile = [](double level)
	{
		core::ProfileSettings profile;
		profile.constantLevel = level;
		return profile;
	};
	auto const runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
	{
		if (world.advanceTicks(ticks)) return;
		auto const diagnostics = world.getStationSystemsDiagnostics();
		std::fprintf(stderr, "%s failed: %s\n", what,
			diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
		throw std::runtime_error(std::string(what) + " run failed");
	};
	auto cropComposition = []()
	{
		core::ResourceCompositionSettings composition;
		composition.moisture = 0.0;
		composition.nutrientNitrogen = 0.05;
		composition.atmosphericNitrogen = 0.0;
		composition.phosphorus = 0.02;
		composition.potassium = 0.03;
		composition.otherMinerals = 0.05;
		composition.energyKJPerKgDry = 1700.0;
		return composition;
	};
	auto massStore = [](std::string id, double capacity, double starting)
	{
		core::ResourceStoreSettings settings;
		settings.id = std::move(id);
		settings.unit = "kg";
		settings.capacity = capacity;
		settings.startingQualified = starting;
		return settings;
	};
	auto habitat = []()
	{
		core::AtmosphereSettings settings;
		settings.id = "habitat";
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
	auto cropRig = [&](double lampKW, double circulationKW)
	{
		core::CropPopulationSettings settings;
		settings.id = "canopy";
		settings.atmosphereId = "habitat";
		settings.waterStoreId = "irrigation";
		settings.nitrogenStoreId = "fertN";
		settings.phosphorusStoreId = "fertP";
		settings.potassiumStoreId = "fertK";
		settings.mineralsStoreId = "fertMin";
		settings.composition = cropComposition();
		settings.growingAreaM2 = 10.0;
		settings.maxBiomassDensityKgPerM2 = 2.0;
		settings.initialBiomassKg = 1.0;
		settings.lampRatedKW = lampKW;
		settings.illuminationProfile = constantProfile(1.0);
		settings.circulationPowerKW = circulationKW;
		settings.growthYieldKgPerKWh = 2.0;
		settings.respirationPerHour = 0.5;
		settings.minGrowingTemperatureC = 10.0;
		settings.maxGrowingTemperatureC = 35.0;
		return settings;
	};
	auto cropStores = [&]()
	{
		std::vector<core::ResourceStoreSettings> stores;
		stores.push_back(massStore("irrigation", 200.0, 150.0));
		stores.push_back(massStore("fertN", 10.0, 5.0));
		stores.push_back(massStore("fertP", 10.0, 5.0));
		stores.push_back(massStore("fertK", 10.0, 5.0));
		stores.push_back(massStore("fertMin", 10.0, 5.0));
		return stores;
	};
	auto discretionaryLoad = [&](std::string id, double demandKW, double maxCurtailment)
	{
		core::EquipmentLoadSettings settings;
		settings.id = std::move(id);
		settings.electricalDemandKW = demandKW;
		settings.operatingProfile = constantProfile(1.0);
		settings.heatOutputKW = 0.0;
		settings.thermalClass = "low";
		settings.coolingDisposition = core::StationCoolingDisposition::Exported;
		settings.criticalFloorKW = 0.0;
		settings.priority = 10;
		settings.maxCurtailment = maxCurtailment;
		return settings;
	};
	auto solarArray = [&](double capacityKW)
	{
		core::SolarArraySettings settings;
		settings.id = "field";
		settings.capacityKW = capacityKW;
		settings.illumination = constantProfile(1.0);
		return settings;
	};
	auto reservoir = [](std::string id, std::string thermalClass, double startingC)
	{
		core::ThermalReservoirSettings settings;
		settings.id = std::move(id);
		settings.thermalClass = std::move(thermalClass);
		settings.heatCapacityKJPerK = 10000.0;
		settings.startingTemperatureC = startingC;
		settings.minTemperatureC = -50.0;
		settings.maxTemperatureC = 200.0;
		return settings;
	};
	auto hazardousLoop = [](std::string id, double pumpKW, double heaterKW, double startingC)
	{
		core::CoolantLoopSettings settings;
		settings.id = std::move(id);
		settings.reservoirId = "sink";
		settings.coolantCapacityKg = 100.0;
		settings.startingCoolantKg = 100.0;
		settings.specificHeatKJPerKgK = 4.0;
		settings.startingTemperatureC = startingC;
		settings.freezeProtectionSetpointC = 5.0;
		settings.pumpPowerKW = pumpKW;
		settings.ratedFlowKgPerS = 1.0;
		settings.exchangerConductanceKWPerK = 0.0;
		settings.heaterCapacityKW = heaterKW;
		return settings;
	};
	auto configure = [](core::World& world, core::StationSystemsSettings settings, char const* what)
	{
		std::string diagnostic;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		require(world.configureStationSystems(settings, false, &diagnostic),
			(std::string(what) + " refused: " + diagnostic).c_str());
	};

	// Scenario 1: lighting curtailment with preserved circulation. The
	// sun leaves less than the canopy's lamp plus the discretionary
	// experiment wants; the essential circulation is admitted whole from
	// the actual pool ahead of the experiment, and only the discretionary
	// lamp is curtailed. Before the protection the experiment's grant
	// could eat into the circulation draw; now it never can.
	{
		core::World world("ProtCrop", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = cropStores();
		settings.atmospheres = { habitat() };
		settings.cropPopulations = { cropRig(4.0, 1.0) };
		settings.solarArrays = { solarArray(4.5) };
		settings.equipmentLoads = { discretionaryLoad("experiment", 4.0, 0.5) };
		configure(world, settings, "lighting curtailment rig");
		world.resumeSimulation();
		runOrDiagnose(world, 10, "lighting curtailment");
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& crop = snap.cropPopulations[0];
		require(closeTo(crop.lastCirculationRequestedKW, 1.0),
			"the canopy circulation demand changed");
		require(closeTo(crop.lastCirculationSuppliedKW, 1.0),
			"the discretionary experiment cut into the protected biological circulation");
		require(crop.lastLightSuppliedKW + 1e-9 < crop.lastLightRequestedKW,
			"the discretionary lamp was not curtailed although the pool was short");
		require(closeTo(snap.power.loads[0].suppliedKW, 3.5),
			"the discretionary load did not take exactly the pool the protected draw left");
		require(closeTo(snap.power.protectedDemandKW, 1.0),
			"the protected demand total is wrong");
		require(closeTo(snap.power.protectedSuppliedKW, 1.0)
			&& closeTo(snap.power.protectedUnmetKW, 0.0),
			"the protected supply total is wrong");
		require(!snap.power.protectedUnmet,
			"the protected alarm raised with no unmet protected demand");
	}

	// Scenario 2: authored-critical shortage. The loop's pump and freeze
	// heater are protected because the author declared `criticalService`
	// (#754) - the hazard class alone protects nothing. With a pool that
	// covers them the circulation runs whole while the discretionary load
	// is squeezed; with a pool below the protected minimum the shortfall
	// stays explicit as unmet critical demand with the reason named and
	// the event logged - never silently reclassified as permissible
	// curtailment.
	{
		core::World world("ProtHaz", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
		auto critical = hazardousLoop("hazLoop", 1.0, 0.5, 0.0);
		critical.criticalService = true;
		settings.coolantLoops = { critical };
		settings.solarArrays = { solarArray(3.0) };
		settings.equipmentLoads = { discretionaryLoad("experiment", 3.0, 0.5) };
		configure(world, settings, "critical shortage rig");
		world.resumeSimulation();
		runOrDiagnose(world, 10, "critical shortage");
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& loop = snap.coolantLoops[0];
		require(closeTo(loop.pumpSuppliedKW, 1.0) && closeTo(loop.heaterSuppliedKW, 0.5),
			"the authored-critical circulation was not protected ahead of the experiment");
		require(closeTo(loop.circulationFraction, 1.0),
			"the protected loop did not circulate fully");
		require(closeTo(snap.power.protectedDemandKW, 1.5)
			&& closeTo(snap.power.protectedSuppliedKW, 1.5),
			"the protection totals are wrong");
		require(!snap.power.protectedUnmet,
			"the protection alarm raised while the pool covered the protected draw");
	}
	{
		core::World world("ProtHazTight", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
		auto critical = hazardousLoop("hazLoop", 1.0, 0.5, 0.0);
		critical.criticalService = true;
		settings.coolantLoops = { critical };
		settings.solarArrays = { solarArray(1.0) };
		settings.equipmentLoads = { discretionaryLoad("experiment", 3.0, 0.5) };
		configure(world, settings, "tight critical rig");
		world.resumeSimulation();
		runOrDiagnose(world, 10, "tight critical");
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& loop = snap.coolantLoops[0];
		require(closeTo(loop.pumpSuppliedKW, 1.0),
			"the protected pump lost supply even though the pool covered it first");
		require(closeTo(loop.heaterSuppliedKW, 0.0),
			"the freeze heater drew beyond the pool the protected pass left");
		require(snap.power.protectedUnmet,
			"the protected shortfall alarm did not raise under the tight pool");
		require(closeTo(snap.power.protectedUnmetKW, 0.5),
			"the unmet protected critical minimum is not the freeze-heater remainder");
		require(sawEvent(world, "Protected containment-circulation 'hazLoop' unmet"),
			"the protected circulation shortfall was not announced");
		require(snap.power.protectedServices[0].reason.find("pool") != std::string::npos,
			"the unmet protected service does not name the binding pool reason");
		require(closeTo(snap.power.loads[0].suppliedKW, 0.0),
			"the discretionary load took power ahead of the protected circulation draw");
	}

	// Scenario 2b: protection is authored, not inferred (#754/#757).
	// A noncritical loop sheds under a brownout while an authored-critical
	// loop keeps circulating. The unprotected loop is declared FIRST so
	// protection cannot come from declaration order.
	{
		core::World world("ProtNotInferred", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
		auto hazNotCritical = hazardousLoop("hazPlain", 1.0, 0.0, 20.0);
		auto critical = hazardousLoop("criticalLoop", 1.0, 0.0, 20.0);
		critical.criticalService = true;
		settings.coolantLoops = { hazNotCritical, critical };
		settings.solarArrays = { solarArray(1.0) };
		configure(world, settings, "authored-not-inferred rig");
		world.resumeSimulation();
		runOrDiagnose(world, 10, "authored-not-inferred brownout");
		auto const& snap = world.getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[1].pumpSuppliedKW, 1.0)
			&& closeTo(snap.coolantLoops[1].circulationFraction, 1.0),
			"the authored-critical loop did not keep circulating through the brownout");
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 0.0)
			&& closeTo(snap.coolantLoops[0].circulationFraction, 0.0)
			&& closeTo(snap.coolantLoops[0].flowKgPerS, 0.0),
			"the noncritical loop was not shed like a discretionary load");
		require(closeTo(snap.power.protectedDemandKW, 1.0)
			&& closeTo(snap.power.protectedSuppliedKW, 1.0)
			&& !snap.power.protectedUnmet,
			"the protected account is not exactly the one authored-critical loop");
		require(snap.power.protectedServices.size() == 1
			&& snap.power.protectedServices[0].id == "criticalLoop",
			"the unprotected loop appeared in the protected-service accounts");
		bool hazAnnounced = false;
		for (auto const& event : world.getStationSystemsEvents())
			if (event.message.find("hazPlain' unmet") != std::string::npos) hazAnnounced = true;
		require(!hazAnnounced,
			"the unprotected loop's shed was announced as unmet protected service");
	}

	// Scenario 3: generator cooling shortfall and shutdown cooling.
	// A generator whose sink is already at the maximum sink temperature
	// cannot run at all - the cooling shortfall is explicit - while the
	// biological circulation stays protected through it.
	{
		core::World world("ProtGenCold", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = cropStores();
		core::ResourceStoreSettings diesel;
		diesel.id = "diesel";
		diesel.unit = "kWh";
		diesel.capacity = 100.0;
		diesel.startingQualified = 50.0;
		settings.stores.push_back(diesel);
		settings.atmospheres = { habitat() };
		settings.cropPopulations = { cropRig(1.0, 1.0) };
		settings.thermalReservoirs = { reservoir("hot", "generation", 95.0) };
		core::GeneratorSettings generator;
		generator.id = "gen";
		generator.fuelStoreId = "diesel";
		generator.thermalClass = "generation";
		generator.heatSinkId = "hot";
		generator.ratedOutputKW = 5.0;
		generator.efficiency = 0.5;
		generator.maxRampKWPerSecond = 5.0;
		generator.commandedOutputKW = 5.0;
		generator.startupPowerKW = 0.0;
		generator.startupSeconds = 0.0;
		generator.shutdownHeatKW = 1.0;
		generator.shutdownSeconds = 10.0;
		generator.maxSinkTemperatureC = 90.0;
		settings.generators = { generator };
		settings.solarArrays = { solarArray(1.5) };
		configure(world, settings, "generator cooling shortfall rig");
		world.resumeSimulation();
		runOrDiagnose(world, 10, "generator cooling shortfall");
		auto const& snap = world.getStationSystemsSnapshot();
		require(!snap.power.generators[0].coolingAvailable,
			"the generator reported qualified cooling into an over-hot sink");
		require(closeTo(snap.power.generators[0].outputKW, 0.0),
			"the generator produced output without qualified cooling");
		require(snap.power.generators[0].blockReason.find("cooling") != std::string::npos,
			"the generator block reason does not name the cooling shortfall");
		require(closeTo(snap.cropPopulations[0].lastCirculationSuppliedKW, 1.0),
			"the biological circulation was not protected through the generator cooling shortfall");
		require(!snap.power.protectedUnmet,
			"the protected alarm raised while the pool covered the circulation");
	}

	// Scenario 3b: shutdown cooling stays a protected obligation after
	// the electrical output stops. The generator burns out its fuel and
	// trips to Shutdown; the decay heat keeps booking and the protected
	// service account shows the standing shutdown-cooling obligation
	// while output is zero - stopping generation never erases the heat.
	{
		core::World world("ProtGenShutdown", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		core::ResourceStoreSettings starveDiesel;
		starveDiesel.id = "diesel";
		starveDiesel.unit = "kWh";
		starveDiesel.capacity = 100.0;
		starveDiesel.startingQualified = 0.005;
		settings.stores = { starveDiesel };
		settings.thermalReservoirs = { reservoir("hot", "generation", 20.0) };
		core::GeneratorSettings generator;
		generator.id = "gen";
		generator.fuelStoreId = "diesel";
		generator.thermalClass = "generation";
		generator.heatSinkId = "hot";
		generator.ratedOutputKW = 5.0;
		generator.efficiency = 0.5;
		generator.maxRampKWPerSecond = 5.0;
		generator.commandedOutputKW = 5.0;
		generator.startupPowerKW = 0.0;
		generator.startupSeconds = 0.0;
		generator.shutdownHeatKW = 2.0;
		generator.shutdownSeconds = 60.0;
		generator.maxSinkTemperatureC = 90.0;
		settings.generators = { generator };
		settings.solarArrays = { solarArray(0.0) };
		configure(world, settings, "shutdown cooling rig");
		world.resumeSimulation();
		runOrDiagnose(world, 240, "shutdown cooling");
		auto const& snap = world.getStationSystemsSnapshot();
		require(snap.power.generators[0].phase == core::StationGeneratorPhase::Shutdown,
			"the fuel-starved generator did not enter Shutdown");
		require(closeTo(snap.power.generators[0].outputKW, 0.0),
			"the generator still reports electrical output after the fuel ran out");
		require(snap.power.generators[0].coolingRequired,
			"the shutdown obligation stopped with the electrical output");
		bool sawShutdownCooling = false;
		for (auto const& service : snap.power.protectedServices)
		{
			if (service.kind != "shutdown-cooling" || service.id != "gen") continue;
			sawShutdownCooling = true;
			require(service.requestedKW > 0.0
				&& closeTo(service.unmetKW, 0.0, 1e-9),
				"the shutdown-cooling protected account is not the booked decay heat");
		}
		require(sawShutdownCooling,
			"the standing shutdown-cooling obligation is not exposed as a protected service");
	}

	// Scenario 4: microbial loss under coordinated operation. The
	// microbial digester competes for the same pool as everything else
	// and is not itself protected; under a tight pool its work is
	// curtailed while the containment circulation ahead of it stays
	// whole - the shortage lands on the discretionary process, never
	// on the protected support services.
	{
		core::World world("ProtMicrobial", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::ResourceCompositionSettings feedComposition;
		feedComposition.moisture = 0.9;
		feedComposition.nutrientNitrogen = 0.05;
		feedComposition.atmosphericNitrogen = 0.02;
		feedComposition.phosphorus = 0.02;
		feedComposition.potassium = 0.02;
		feedComposition.otherMinerals = 0.01;
		feedComposition.energyKJPerKgDry = 8000.0;
		core::ResourceStoreSettings feed;
		feed.id = "h_waste";
		feed.unit = "kg";
		feed.capacity = 100.0;
		feed.startingQualified = 50.0;
		feed.composition = feedComposition;
		core::StationSystemsSettings settings;
		settings.stores = { feed,
			massStore("h_oxygen", 100.0, 50.0),
			massStore("h_water", 100.0, 50.0),
			massStore("h_media", 100.0, 50.0),
			massStore("h_nutrients", 100.0, 0.0),
			massStore("h_recoveredWater", 100.0, 0.0),
			massStore("h_co2", 100.0, 0.0),
			massStore("h_residue", 100.0, 0.0),
			massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
		auto critical = hazardousLoop("hazLoop", 1.0, 0.0, 20.0);
		critical.criticalService = true;
		settings.coolantLoops = { critical };
		core::MicrobialProcessorSettings digester;
		digester.id = "digester";
		digester.feedStoreId = "h_waste";
		digester.oxygenStoreId = "h_oxygen";
		digester.waterStoreId = "h_water";
		digester.consumableStoreId = "h_media";
		digester.nutrientStoreId = "h_nutrients";
		digester.recoveredWaterStoreId = "h_recoveredWater";
		digester.co2StoreId = "h_co2";
		digester.residueStoreId = "h_residue";
		digester.thermalClass = "low";
		digester.heatSinkId = "sink";
		digester.rateKgPerS = 0.001;
		digester.specificEnergyKWhPerKgFeed = 2.0;
		digester.oxygenPerKgFeedKg = 0.8;
		digester.processWaterPerKgFeedKg = 0.5;
		digester.consumablePerKgFeed = 0.01;
		digester.co2PerKgFeedKg = 0.3;
		digester.nitrogenRecovery = 0.8;
		digester.phosphorusRecovery = 0.8;
		digester.potassiumRecovery = 0.8;
		digester.mineralsRecovery = 0.7;
		digester.waterRecovery = 0.7;
		digester.reactionHeatKJPerKgFeed = 600.0;
		digester.minOperatingTemperatureC = 15.0;
		digester.maxSinkTemperatureC = 45.0;
		settings.microbialProcessors = { digester };
		settings.solarArrays = { solarArray(7.0) };
		configure(world, settings, "microbial coordination rig");
		world.resumeSimulation();
		runOrDiagnose(world, 10, "microbial coordination");
		auto const& snap = world.getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 1.0),
			"the authored-critical circulation was not protected ahead of the digester");
		auto const& processor = snap.microbialProcessors[0];
		require(processor.lastPowerDemandKW > 1.0,
			"the digester declared no meaningful electrical demand to be curtailed");
		require(processor.lastPowerSuppliedKW + 1e-9 < processor.lastPowerDemandKW,
			"the digester was not curtailed although the pool was short after the protected draw");
		require(!snap.power.protectedUnmet,
			"the protected alarm raised while the protected draw was fully supplied");
	}

	// Scenario 5: competing protected dependencies resolve
	// deterministically by declaration order. The pool covers the first
	// authored-critical loop whole and part of the second; the second
	// loop's remainder stays unmet critical demand and is announced, while
	// the first loop's event log stays clean. Two identical runs agree.
	auto competingRun = [&]() -> std::unique_ptr<core::World>
	{
		auto world = std::make_unique<core::World>("ProtCompete", 2, 2);
		world->pauseSimulation();
		world->finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
		auto loopA = hazardousLoop("loopA", 2.0, 0.0, 20.0);
		auto loopB = hazardousLoop("loopB", 2.0, 0.0, 20.0);
		loopA.criticalService = true;
		loopB.criticalService = true;
		settings.coolantLoops = { loopA, loopB };
		settings.solarArrays = { solarArray(3.0) };
		std::string diagnostic;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		if (!world->configureStationSystems(settings, false, &diagnostic))
			throw std::runtime_error("competing protected rig refused: " + diagnostic);
		world->resumeSimulation();
		if (!world->advanceTicks(10))
			throw std::runtime_error("competing protected run failed");
		return world;
	};
	{
		auto world = competingRun();
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 2.0),
			"the first-declared protected loop did not get its full draw");
		require(closeTo(snap.coolantLoops[1].pumpSuppliedKW, 1.0),
			"the second protected loop did not get exactly the pool remainder");
		require(snap.power.protectedUnmet && closeTo(snap.power.protectedUnmetKW, 1.0),
			"the competing protected shortfall is not the second loop's unmet minimum");
		require(sawEvent(*world, "Protected containment-circulation 'loopB' unmet"),
			"the second loop's protected shortfall was not announced");
		bool firstLoopAnnounced = false;
		for (auto const& event : world->getStationSystemsEvents())
			if (event.message.find("loopA' unmet") != std::string::npos) firstLoopAnnounced = true;
		require(!firstLoopAnnounced,
			"the fully supplied first loop was announced as unmet");
		auto const firstUnmet = snap.power.protectedUnmetKW;
		auto const second = competingRun();
		require(closeTo(second->getStationSystemsSnapshot().power.protectedUnmetKW, firstUnmet),
			"competing protected allocation was not deterministic");
	}

	// Scenario 5b: critical chiller circulation dependency (#579).
	// A critical-service chiller on an ordinary, unprotected coolant loop
	// depends on that loop's pump for any cooling it can deliver, so the
	// protected pass funds the pump ahead of the chiller's own work and
	// ahead of every discretionary load, and sizes the cooling credit by
	// the circulation the pool actually powers - never by the loop's
	// fill and outage capacity alone. The loop needs no manual
	// criticalService flag for its circulation to be protected, and a
	// pump the pool cannot power leaves the cooling that could not be
	// delivered explicit as unmet critical service naming the
	// circulation as the binding dependency.
	auto normalLoop = [](std::string id, double pumpKW, double ratedFlowKgPerS)
	{
		core::CoolantLoopSettings settings;
		settings.id = std::move(id);
		settings.reservoirId = "sink";
		settings.coolantCapacityKg = 1.0;
		settings.startingCoolantKg = 1.0;
		settings.specificHeatKJPerKgK = 1.0;
		settings.startingTemperatureC = 20.0;
		settings.freezeProtectionSetpointC = 5.0;
		settings.pumpPowerKW = pumpKW;
		settings.ratedFlowKgPerS = ratedFlowKgPerS;
		settings.exchangerConductanceKWPerK = 0.0;
		settings.heaterCapacityKW = 0.0;
		return settings;
	};
	auto criticalChiller = [](std::string id, std::string loopId)
	{
		core::ChillerSettings settings;
		settings.id = std::move(id);
		settings.thermalClass = "low";
		settings.coldLoopId = std::move(loopId);
		settings.condenserReservoirId = "hot";
		settings.ratedCoolingCapacityKW = 2.0;
		settings.cop = 1.0;
		settings.auxiliaryPowerKW = 0.0;
		settings.chilledSetpointC = 5.0;
		settings.maxCondensingTemperatureC = 45.0;
		settings.criticalService = true;
		return settings;
	};
	auto dependencyRun = [&](double solarKW, double ratedFlowKgPerS,
		bool fullLoopOutage = false)
	{
		auto world = std::make_unique<core::World>("ProtChiller", 2, 2);
		world->pauseSimulation();
		world->finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0),
			reservoir("hot", "low", 20.0) };
		settings.coolantLoops = { normalLoop("loop", 1.0, ratedFlowKgPerS) };
		settings.chillers = { criticalChiller("chiller", "loop") };
		settings.solarArrays = { solarArray(solarKW) };
		settings.equipmentLoads = { discretionaryLoad("experiment", 1.0, 1.0) };
		if (fullLoopOutage)
		{
			core::InstallationOutageSettings outage;
			outage.id = "loopOut";
			outage.installationId = "loop";
			outage.capacityFraction = 0.0;
			outage.firstOccurrenceSeconds = 0.0;
			outage.durationSeconds = 10.0;
			settings.outages = { outage };
		}
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		std::string diagnostic;
		if (!world->configureStationSystems(settings, false, &diagnostic))
			throw std::runtime_error("chiller dependency rig refused: " + diagnostic);
		world->resumeSimulation();
		runOrDiagnose(*world, 1, "chiller dependency run");
		return world;
	};
	auto findProtectedService = [](core::StationSystemsSnapshot const& snap,
		std::string const& id, std::string const& kind) -> core::StationProtectedServiceSnapshot const*
	{
		for (auto const& service : snap.power.protectedServices)
			if (service.id == id && service.kind == kind) return &service;
		return nullptr;
	};
	// A tight but partially usable pool: the pump is funded whole first,
	// the chiller converts what the pool leaves into real cooling, and the
	// remainder of the protected cooling demand stays unmet with the
	// pool named as the binding constraint. The competing discretionary
	// load gets nothing ahead of the protected bundle.
	{
		auto world = dependencyRun(2.0, 1.0);
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 1.0),
			"the unprotected loop's pump was not protected as the critical chiller's dependency");
		require(closeTo(snap.coolantLoops[0].circulationFraction, 1.0),
			"the dependency-protected loop did not circulate fully");
		require(closeTo(snap.chillers[0].powerDemandKW, 2.0)
			&& closeTo(snap.chillers[0].powerSuppliedKW, 1.0),
			"the chiller did not consume exactly the pool the protected pump draw left");
		require(closeTo(snap.chillers[0].coolingKW, 1.0),
			"the chiller's delivered cooling does not match the work actually powered");
		require(closeTo(snap.power.protectedDemandKW, 3.0)
			&& closeTo(snap.power.protectedSuppliedKW, 2.0)
			&& closeTo(snap.power.protectedUnmetKW, 1.0),
			"the protected bundle totals do not account for pump plus curtailed cooling work");
		require(snap.power.protectedUnmet,
			"the protected alarm did not raise with unmet critical cooling");
		auto const* pumpService = findProtectedService(snap, "loop",
			"critical-cooling-circulation");
		require(pumpService != nullptr
			&& closeTo(pumpService->requestedKW, 1.0)
			&& closeTo(pumpService->suppliedKW, 1.0),
			"the chiller's circulation dependency is not exposed as a protected service");
		auto const* coolingService = findProtectedService(snap, "chiller",
			"critical-cooling");
		require(coolingService != nullptr
			&& closeTo(coolingService->requestedKW, 2.0)
			&& closeTo(coolingService->suppliedKW, 1.0)
			&& closeTo(coolingService->unmetKW, 1.0),
			"the curtailed protected cooling demand is not exposed with its unmet remainder");
		require(coolingService != nullptr
			&& coolingService->reason.find("pool") != std::string::npos,
			"the curtailed cooling does not name the binding pool");
		require(sawEvent(*world, "Protected critical-cooling 'chiller' unmet"),
			"the unmet protected cooling was not announced");
		require(closeTo(snap.power.loads[0].suppliedKW, 0.0),
			"the discretionary load took power ahead of the protected chiller bundle");
	}
	// A pool that cannot even fully power the pump: the circulation
	// itself is the binding dependency, the cooling the unpowered
	// circulation cannot deliver stays explicit unmet critical service,
	// and no pool power is wasted on work the chiller cannot use.
	{
		auto world = dependencyRun(0.5, 1.0);
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 0.5),
			"the dependency pump draw did not take the whole pool first");
		require(closeTo(snap.chillers[0].coolingKW, 0.0)
			&& closeTo(snap.chillers[0].powerSuppliedKW, 0.0),
			"cooling work was funded beyond the circulation the pool could power");
		auto const* pumpService = findProtectedService(snap, "loop",
			"critical-cooling-circulation");
		require(pumpService != nullptr
			&& closeTo(pumpService->requestedKW, 1.0)
			&& closeTo(pumpService->suppliedKW, 0.5)
			&& closeTo(pumpService->unmetKW, 0.5),
			"the curtailed dependency pump draw is not exposed with its unmet remainder");
		auto const* coolingService = findProtectedService(snap, "chiller",
			"critical-cooling");
		require(coolingService != nullptr
			&& closeTo(coolingService->requestedKW, 2.0)
			&& closeTo(coolingService->suppliedKW, 0.0)
			&& closeTo(coolingService->unmetKW, 2.0),
			"the cooling blocked by unpowered circulation is not explicit unmet critical service");
		require(coolingService != nullptr
			&& coolingService->reason.find("circulation") != std::string::npos,
			"the binding circulation dependency is not named in the unmet cooling reason");
		require(sawEvent(*world, "Protected critical-cooling-circulation 'loop' unmet"),
			"the curtailed dependency pump draw was not announced");
		require(sawEvent(*world, "Protected critical-cooling 'chiller' unmet"),
			"the circulation-blocked cooling was not announced");
		require(closeTo(snap.power.protectedDemandKW, 3.0)
			&& closeTo(snap.power.protectedSuppliedKW, 0.5)
			&& closeTo(snap.power.protectedUnmetKW, 2.5),
			"the protected totals do not expose the circulation-blocked shortfall");
	}
	// A zero-rated-flow loop is a zero-circulation machine: the pump may
	// still run and dissipate as the loop pass powers it (#558), but no
	// coolant moves, so no protected power is drawn for a draw that buys
	// no circulation, and the whole cooling demand stays unmet naming the
	// missing circulation.
	{
		auto world = dependencyRun(2.0, 0.0);
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].circulationFraction, 0.0),
			"a zero-rated-flow loop reported circulation it cannot move");
		require(closeTo(snap.power.loads[0].suppliedKW, 1.0),
			"the discretionary load did not take the pool the unprotectable zero-flow pump left");
		require(findProtectedService(snap, "loop", "critical-cooling-circulation") == nullptr,
			"a zero-flow loop's circulation draw was protected although it buys no circulation");
		auto const* coolingService = findProtectedService(snap, "chiller",
			"critical-cooling");
		require(coolingService != nullptr
			&& closeTo(coolingService->requestedKW, 2.0)
			&& closeTo(coolingService->suppliedKW, 0.0)
			&& closeTo(coolingService->unmetKW, 2.0),
			"zero-flow cooling was not explicit unmet critical service");
		require(coolingService != nullptr
			&& coolingService->reason.find("circulation") != std::string::npos,
			"the zero-flow unmet cooling does not name the circulation dependency");
		require(closeTo(snap.chillers[0].coolingKW, 0.0),
			"cooling was delivered on a zero-flow loop");
	}
	// A full outage of the cold loop leaves no deliverable capacity and
	// no circulation dependency to fund: the chiller draws no protected
	// power, delivers no cooling, and its own service account names the
	// missing circulation as the refusal.
	{
		auto world = dependencyRun(2.0, 1.0, true);
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 0.0),
			"the outaged loop's pump drew power");
		require(findProtectedService(snap, "loop", "critical-cooling-circulation") == nullptr
			&& findProtectedService(snap, "chiller", "critical-cooling") == nullptr,
			"a fully outaged chiller bundle presented protected demand");
		require(closeTo(snap.chillers[0].coolingKW, 0.0)
			&& closeTo(snap.chillers[0].powerSuppliedKW, 0.0),
			"the outaged-loop chiller drew power or delivered cooling");
		require(snap.chillers[0].blockReason.find("circulation") != std::string::npos,
			"the outaged-loop chiller refusal does not name the missing circulation");
	}
	// Determinism: two identical runs of the tight-pool dependency rig
	// agree on every protected and delivered scalar.
	{
		auto const first = dependencyRun(2.0, 1.0);
		auto const second = dependencyRun(2.0, 1.0);
		auto const& a = first->getStationSystemsSnapshot();
		auto const& b = second->getStationSystemsSnapshot();
		require(closeTo(a.power.protectedDemandKW, b.power.protectedDemandKW)
			&& closeTo(a.power.protectedSuppliedKW, b.power.protectedSuppliedKW)
			&& closeTo(a.power.protectedUnmetKW, b.power.protectedUnmetKW)
			&& closeTo(a.coolantLoops[0].pumpSuppliedKW, b.coolantLoops[0].pumpSuppliedKW)
			&& closeTo(a.chillers[0].coolingKW, b.chillers[0].coolingKW)
			&& closeTo(a.chillers[0].powerSuppliedKW, b.chillers[0].powerSuppliedKW),
			"the protected chiller dependency allocation was not deterministic");
	}

	// Scenario 5c: protected chiller credit reconciliation (#580).
	// The protected cooling credit is sized from the loop's tick-start
	// heat, but the loop's implicit exchange with its reservoir is only
	// solved later in the tick, so the credit is a reservation, not
	// spent work: every watt the real duty cannot use must return to
	// the source it came from, and the battery ledger, the generation
	// surplus, the competing consumers and the protected requested/
	// supplied/unmet account must all agree on the work that actually
	// moved. A critical chiller on a critical loop whose cold reservoir
	// drags the extractable heat to zero (or near it) must strand
	// nothing on the bus.
	auto creditRun = [&](double solarKW, double conductanceKWPerK,
		bool withBattery, bool withCompetitor)
	{
		auto world = std::make_unique<core::World>("ChillerCredit", 2, 2);
		world->pauseSimulation();
		world->finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", -20.0),
			reservoir("hot", "low", 20.0) };
		auto loop = normalLoop("loop", 1.0, 1.0);
		loop.criticalService = true;
		loop.exchangerConductanceKWPerK = conductanceKWPerK;
		settings.coolantLoops = { loop };
		settings.chillers = { criticalChiller("chiller", "loop") };
		if (solarKW > 0.0) settings.solarArrays = { solarArray(solarKW) };
		if (withBattery)
		{
			core::BatterySettings bank;
			bank.id = "bank";
			bank.nameplateKWh = 10.0;
			bank.usableKWh = 9.0;
			bank.startingStoredKWh = 5.0;
			bank.maxChargeKW = 5.0;
			bank.maxDischargeKW = 10.0;
			bank.dischargeEfficiency = 0.9;
			settings.batteries = { bank };
		}
		if (withCompetitor)
			settings.equipmentLoads = { discretionaryLoad("experiment", 1.0, 1.0) };
		configure(*world, settings, "chiller credit rig");
		world->resumeSimulation();
		runOrDiagnose(*world, 1, "chiller credit run");
		return world;
	};
	auto const tickHours = static_cast<double>(core::World::getFixedTimestep()) / 3600.0;
	// The exchange leaves the cold loop no extractable heat: the whole
	// 2 kW work reservation has no consumer, so it returns to the bus
	// as surplus, the protected account reports only the pump as
	// supplied, and the competing discretionary load never saw those
	// watts spent.
	{
		auto world = creditRun(3.0, 1.0e6, false, true);
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 1.0),
			"the protected pump did not run in the credit reconciliation rig");
		require(closeTo(snap.chillers[0].coolingKW, 0.0)
			&& closeTo(snap.chillers[0].powerSuppliedKW, 0.0),
			"the exchange-blocked chiller consumed work it could not convert");
		require(closeTo(snap.power.protectedDemandKW, 3.0)
			&& closeTo(snap.power.protectedSuppliedKW, 1.0)
			&& closeTo(snap.power.protectedUnmetKW, 2.0),
			"the protected account still counts the unused reservation as supplied work");
		require(snap.power.protectedUnmet,
			"the reconciled unmet cooling did not raise the protected alarm");
		auto const* cooling = findProtectedService(snap, "chiller", "critical-cooling");
		require(cooling != nullptr
			&& closeTo(cooling->requestedKW, 2.0)
			&& closeTo(cooling->suppliedKW, 0.0)
			&& closeTo(cooling->unmetKW, 2.0)
			&& cooling->reason.find("returned to the pool") != std::string::npos,
			"the reconciled cooling service does not expose the returned credit");
		// Whole-bus energy balance: solar in = pump + chiller work +
		// surplus; the unused credit is back on the bus, not stranded.
		require(closeTo(snap.power.surplusKW, 2.0)
			&& closeTo(snap.power.availableGenerationKW,
				snap.coolantLoops[0].pumpSuppliedKW
				+ snap.chillers[0].powerSuppliedKW + snap.power.surplusKW),
			"the whole-bus energy balance does not close on the returned credit");
		require(closeTo(snap.power.loads[0].suppliedKW, 0.0),
			"the competing discretionary load received power in the reconciliation rig");
		require(sawEvent(*world, "Protected critical-cooling 'chiller' unmet"),
			"the reconciled unused cooling was not announced as unmet");
	}
	// The exchange leaves a small but nonzero duty: the credit funds the
	// duty that really moves, only the remainder returns, and every
	// ledger agrees on that one number.
	{
		auto world = creditRun(3.0, 36.0, false, true);
		auto const& snap = world->getStationSystemsSnapshot();
		auto const work = snap.chillers[0].powerSuppliedKW;
		require(work > 0.5 && work < 2.0,
			"the exchange-limited duty is not a smaller-but-nonzero share of the credit");
		require(closeTo(snap.chillers[0].coolingKW, work),
			"the cooling delivered does not match the work actually powered");
		require(closeTo(snap.power.protectedSuppliedKW, 1.0 + work)
			&& closeTo(snap.power.protectedUnmetKW, 2.0 - work),
			"the protected account does not agree with the work that actually moved");
		require(closeTo(snap.power.surplusKW, 2.0 - work)
			&& closeTo(snap.power.availableGenerationKW,
				snap.coolantLoops[0].pumpSuppliedKW + work + snap.power.surplusKW),
			"the whole-bus energy balance does not close on the partially used credit");
		require(closeTo(snap.power.loads[0].suppliedKW, 0.0),
			"the competing discretionary load took power ahead of the protected bundle");
	}
	// A battery-funded credit whose duty is zero: the refund goes back
	// into storage, not just off the discharge report - the battery paid
	// only for the pump, the work it never supplied never left storage.
	{
		auto world = creditRun(0.0, 1.0e6, true, false);
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 1.0)
			&& closeTo(snap.chillers[0].powerSuppliedKW, 0.0),
			"the battery-funded reconciliation rig did not run as authored");
		auto const& bank = snap.power.batteries[0];
		require(closeTo(bank.lastDischargeKW, 1.0),
			"the refunded battery credit still counted as discharge this tick");
		require(closeTo(bank.storedKWh, 5.0 - 1.0 * tickHours / 0.9),
			"the refunded battery energy did not return to storage");
		require(closeTo(bank.dischargedKWh, 1.0 * tickHours / 0.9)
			&& closeTo(bank.deliveredKWh, 1.0 * tickHours),
			"the battery energy ledger does not close on the reconciled credit");
		require(closeTo(snap.power.protectedSuppliedKW, 1.0)
			&& closeTo(snap.power.surplusKW, 0.0),
			"the protected and surplus accounts disagree with the battery-funded reconciliation");
	}
	// A battery-funded credit with a smaller-but-nonzero duty: the
	// battery discharges exactly the pump plus the work that moved, and
	// the unused remainder of its draw is back in storage.
	{
		auto world = creditRun(0.0, 36.0, true, false);
		auto const& snap = world->getStationSystemsSnapshot();
		auto const work = snap.chillers[0].powerSuppliedKW;
		require(work > 0.5 && work < 2.0,
			"the battery-funded exchange-limited duty is not smaller but nonzero");
		auto const& bank = snap.power.batteries[0];
		require(closeTo(bank.lastDischargeKW, 1.0 + work),
			"the battery still discharged the unused part of the protected credit");
		require(closeTo(bank.storedKWh, 5.0 - (1.0 + work) * tickHours / 0.9),
			"the unused battery-funded credit did not return to storage");
		require(closeTo(snap.power.protectedSuppliedKW, 1.0 + work)
			&& closeTo(snap.power.protectedUnmetKW, 2.0 - work),
			"the protected account disagrees with the battery-funded actual work");
		require(closeTo(snap.power.surplusKW, 0.0),
			"a battery-funded reconciliation reported surplus it does not have");
	}
	// Determinism: two identical reconciliation runs agree on every
	// protected, delivered and battery scalar.
	{
		auto const first = creditRun(0.0, 36.0, true, true);
		auto const second = creditRun(0.0, 36.0, true, true);
		auto const& a = first->getStationSystemsSnapshot();
		auto const& b = second->getStationSystemsSnapshot();
		require(closeTo(a.power.protectedSuppliedKW, b.power.protectedSuppliedKW)
			&& closeTo(a.chillers[0].powerSuppliedKW, b.chillers[0].powerSuppliedKW)
			&& closeTo(a.power.batteries[0].storedKWh, b.power.batteries[0].storedKWh)
			&& closeTo(a.power.surplusKW, b.power.surplusKW),
			"the protected chiller credit reconciliation was not deterministic");
	}
	// Host/Lua seam: the bundled sandbox refunds the unused credit in
	// its own battery ledger and protected account, and both agree with
	// the host snapshot for the same tick.
	{
		SourceScope scope;
		auto const bundled = core::bundledStationSystemsSource();
		core::setStationSystemsSourceLoader([bundled] {
			return "local factory = (function()\n" + bundled + "\nend)()\n"
				"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, cropPopulations, algaeCultures, outages, dispatchMode)\n"
				"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, cropPopulations, algaeCultures, outages, dispatchMode)\n"
				"local original = model.advance\n"
				"function model.advance(self, world_tick, fixed_seconds)\n"
				"local result = original(self, world_tick, fixed_seconds)\n"
				"local chiller = self.chillers[1]\n"
				"local bank = self.batteries[1]\n"
				"local hours = fixed_seconds / 3600.0\n"
				"if world_tick == 1 then\n"
				"assert(math.abs(chiller.lastPowerSupplied) < 1e-12, 'sandbox chiller spent credit it could not convert')\n"
				"assert(math.abs(self.protectedSupplied - 1.0) < 1e-12, 'sandbox protected account not reconciled to actual work')\n"
				"assert(math.abs(self.protectedUnmet - 2.0) < 1e-12, 'sandbox protected unmet not reconciled')\n"
				"assert(math.abs(bank.last_discharge - 1.0) < 1e-12, 'sandbox battery still discharged the refunded credit')\n"
				"assert(math.abs(bank.stored - (5.0 - 1.0 * hours / 0.9)) < 1e-12, 'sandbox battery energy not returned to storage')\n"
				"else\n"
				"assert(math.abs(self.protectedSupplied - (1.0 + chiller.lastPowerSupplied)) < 1e-12, 'sandbox protected account disagrees with the actual work')\n"
				"assert(math.abs(bank.last_discharge - (1.0 + chiller.lastPowerSupplied)) < 1e-12, 'sandbox battery discharge disagrees with the actual work')\n"
				"end\n"
				"return result\n"
				"end\n"
				"return model\n"
				"end";
		});
		auto world = creditRun(0.0, 1.0e6, true, false);
		// The host ledger over the same tick agrees with what the sandbox
		// asserted for its own battery and protected accounts.
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.power.protectedSuppliedKW, 1.0)
			&& closeTo(snap.power.batteries[0].lastDischargeKW, 1.0),
			"the host and sandbox chiller credit reconciliation disagree");
	}

	// Scenario 6: authored persistence. The protected flags round-trip
	// with the World document, and a pre-92 document carrying them is
	// refused rather than silently dropping the protection.
	{
		core::World world("ProtPersist", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0),
			reservoir("hot", "low", 20.0) };
		auto loop = hazardousLoop("hazLoop", 1.0, 0.0, 20.0);
		loop.criticalService = true;
		settings.coolantLoops = { loop };
		core::ChillerSettings chiller;
		chiller.id = "chiller";
		chiller.thermalClass = "low";
		chiller.coldLoopId = "hazLoop";
		chiller.condenserReservoirId = "hot";
		chiller.ratedCoolingCapacityKW = 5.0;
		chiller.cop = 3.0;
		chiller.auxiliaryPowerKW = 0.2;
		chiller.chilledSetpointC = 5.0;
		chiller.maxCondensingTemperatureC = 45.0;
		chiller.criticalService = true;
		settings.chillers = { chiller };
		settings.solarArrays = { solarArray(10.0) };
		configure(world, settings, "protected persistence rig");
		auto const text = yaml(world);
		require(text.find("criticalService: true") != std::string::npos,
			"the protected service flag was not saved");
		core::SerializationWorkData data;
		auto reader = core::YamlSerializer::fromString(text);
		reader->deserialize();
		core::World loaded("ProtPersistLoaded", 2, 2);
		loaded.deserialize(*reader, data);
		require(loaded.getStationSystemsSettings() == world.getStationSystemsSettings(),
			"protected service flags did not round-trip");
		require(loaded.getStationSystemsSnapshot().worldTick == 0,
			"the loaded world restored live station state instead of the authored start");
		auto stale = text;
		require(stale.find("version: 103") != std::string::npos,
			"the fixture document carries no version line");
		stale.replace(stale.find("version: 103"), std::string("version: 103").size(),
			"version: 91");
		bool refused = false;
		try
		{
			auto staleReader = core::YamlSerializer::fromString(stale);
			staleReader->deserialize();
			core::World staleWorld("ProtPersistStale", 2, 2);
			staleWorld.deserialize(*staleReader, data);
		}
		catch (std::exception const& error)
		{
			refused = true;
			require(std::string(error.what()).find("schema 92") != std::string::npos,
				"the pre-92 protected-flag refusal does not name the required schema");
		}
		require(refused, "a pre-92 document carrying protected service flags was silently accepted");
	}
}

// #417: service recovery staging. A purely discretionary load that
// falls below its minimum operable supply is held Shed until the declared
// dwell, hysteresis, cooling and firm-source gate reopens it, so a
// capacity that flickers across the threshold cannot toggle the
// equipment on and off. A restart draws its declared startup surge on
// top of normal demand, bounded by the actual pool; the pending restart
// headroom is held out of battery charging so reserves refill only from
// surplus the recovery does not need. All scenarios drive the existing
// World tick/snapshot seam and read the recovery stage off the snapshot.
void recovery(smoke::Context const& context)
{
	auto const closeTo = [](double actual, double expected, double tolerance = 1e-6)
	{ return std::abs(actual - expected) <= tolerance; };
	auto const constantProfile = [](double level)
	{ core::ProfileSettings p; p.constantLevel = level; return p; };
	auto const stepProfile = [](double period, std::vector<std::pair<double, double>> segments)
	{
		core::ProfileSettings p;
		p.periodSeconds = period;
		for (auto const& segment : segments) p.segments.push_back({segment.first, segment.second});
		return p;
	};
	auto const runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
	{
		if (world.advanceTicks(ticks)) return;
		auto const diagnostics = world.getStationSystemsDiagnostics();
		std::fprintf(stderr, "%s failed: %s\n", what,
			diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
		throw std::runtime_error(std::string(what) + " run failed");
	};
	auto const massStore = [](std::string id, double capacity, double starting)
	{
		core::ResourceStoreSettings s; s.id = std::move(id); s.unit = "kg";
		s.capacity = capacity; s.startingQualified = starting; return s;
	};
	auto const configure = [](core::World& world, core::StationSystemsSettings settings, char const* what)
	{
		std::string diagnostic; settings.enabled = true; settings.observationIntervalSeconds = 1.0f;
		require(world.configureStationSystems(settings, false, &diagnostic),
			(std::string(what) + " refused: " + diagnostic).c_str());
	};
	auto const recoveryLoad = [&](std::string id, double demandKW, double maxCurtailment,
		double dwell, double hysteresis, double surgeKW, double surgeSeconds, double heatKW)
	{
		core::EquipmentLoadSettings s;
		s.id = std::move(id);
		s.electricalDemandKW = demandKW;
		s.operatingProfile = constantProfile(1.0);
		s.heatOutputKW = heatKW;
		s.thermalClass = "low";
		s.coolingDisposition = core::StationCoolingDisposition::Exported;
		s.criticalFloorKW = 0.0;
		s.priority = 10;
		s.maxCurtailment = maxCurtailment;
		s.recoveryDwellSeconds = dwell;
		s.recoveryHysteresisFraction = hysteresis;
		s.restartSurgePowerKW = surgeKW;
		s.restartSurgeSeconds = surgeSeconds;
		return s;
	};
	auto const solar = [](double capacity, core::ProfileSettings illum)
	{
		core::SolarArraySettings s; s.id = "field"; s.capacityKW = capacity; s.illumination = illum; return s;
	};
	auto const reservoir = [](std::string id, std::string thermalClass, double startingC, double maxC)
	{
		core::ThermalReservoirSettings s; s.id = std::move(id); s.thermalClass = std::move(thermalClass);
		s.heatCapacityKJPerK = 10000.0; s.startingTemperatureC = startingC;
		s.minTemperatureC = -50.0; s.maxTemperatureC = maxC; return s;
	};
	auto const battery = [](std::string id, double nameplate, double usable, double starting)
	{
		core::BatterySettings s; s.id = std::move(id); s.nameplateKWh = nameplate; s.usableKWh = usable;
		s.startingStoredKWh = starting; s.maxChargeKW = 100.0; s.maxDischargeKW = 100.0; return s;
	};
	auto loadSnap = [](core::World& world, std::string const& id) -> core::StationEquipmentLoadSnapshot
	{
		for (auto const& l : world.getStationSystemsSnapshot().power.loads)
			if (l.id == id) return l;
		throw std::runtime_error("load not found: " + id);
	};

	// Scenario A: shed on shortage, hold through the dwell even after the
	// capacity returns, restart once dwell + hysteresis clear, and repeat
	// the whole cycle on the next low-sun half without drift.
	{
		core::World world("RecA", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		// capacity 20 kW: level 0.2 -> 4 kW (below the 10 kW min), level 0.75 -> 15 kW (above the 12 kW restart threshold).
		settings.solarArrays = { solar(20.0, stepProfile(100.0, {{0.0, 0.2}, {50.0, 0.75}})) };
		settings.equipmentLoads = { recoveryLoad("rig", 10.0, 0.0, 5.0, 0.2, 0.0, 0.0, 0.0) };
		configure(world, settings, "recovery A rig");
		world.resumeSimulation();
		runOrDiagnose(world, 1500, "recovery A low sun"); // t = 25s
		{
			auto const& l = loadSnap(world, "rig");
			require(l.recoveryStage == core::StationLoadRecoveryStage::Shed,
				"the load did not shed under the low-sun shortage");
			require(l.suppliedKW <= 1e-9, "a shed load drew power");
			require(l.recoveryHoldReason.find("hysteresis") != std::string::npos,
				"the shed load was not held by the hysteresis headroom");
		}
		runOrDiagnose(world, 1560, "recovery A high sun"); // t = 51s
		{
			auto const& l = loadSnap(world, "rig");
			require(l.recoveryStage == core::StationLoadRecoveryStage::Active,
				"the load did not restart once the dwell and hysteresis cleared");
			require(closeTo(l.suppliedKW, 10.0), "the restarted load is not fully supplied");
		}
		runOrDiagnose(world, 4440, "recovery A second low sun"); // t = 125s
		require(loadSnap(world, "rig").recoveryStage == core::StationLoadRecoveryStage::Shed,
			"the load did not shed again on the second low-sun half (repeated incident)");
		runOrDiagnose(world, 1560, "recovery A second high sun"); // t = 151s
		require(loadSnap(world, "rig").recoveryStage == core::StationLoadRecoveryStage::Active,
			"the load did not recover again on the second high-sun half (repeated incident)");
	}

	// Scenario B: a declared startup surge is drawn on top of normal
	// demand through a Restoring stage, then the load settles to Active
	// once the surge duration has elapsed.
	{
		core::World world("RecB", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(20.0, stepProfile(100.0, {{0.0, 0.15}, {50.0, 0.75}})) };
		// demand 10, min 10, dwell 0, hysteresis 0.1 (restart needs 11), surge 5 kW for 2s.
		settings.equipmentLoads = { recoveryLoad("surger", 10.0, 0.0, 0.0, 0.1, 5.0, 2.0, 0.0) };
		configure(world, settings, "recovery B rig");
		world.resumeSimulation();
		runOrDiagnose(world, 1500, "recovery B low sun"); // t = 25s
		require(loadSnap(world, "surger").recoveryStage == core::StationLoadRecoveryStage::Shed,
			"the surging load did not shed under the low-sun shortage");
		runOrDiagnose(world, 1560, "recovery B restart"); // t = 51s, mid-Restoring
		{
			auto const& l = loadSnap(world, "surger");
			require(l.recoveryStage == core::StationLoadRecoveryStage::Restoring,
				"the load is not in the Restoring stage mid-surge");
			require(closeTo(l.recoverySurgeKW, 5.0), "the startup surge was not drawn at the declared rate");
			require(closeTo(l.suppliedKW, 10.0), "the Restoring load's normal demand was not supplied");
			require(l.recoverySecondsRemaining > 0.0 && l.recoverySecondsRemaining <= 2.0,
				"the surge countdown is not tracking the declared surge duration");
		}
		runOrDiagnose(world, 180, "recovery B settle"); // t = 54s, surge done
		{
			auto const& l = loadSnap(world, "surger");
			require(l.recoveryStage == core::StationLoadRecoveryStage::Active,
				"the load did not settle to Active after the surge elapsed");
			require(closeTo(l.recoverySurgeKW, 0.0), "surge still drawn after the load went Active");
		}
	}

	// Scenario C: a heat-producing load will not restart on a thin firm-
	// generation supply - the firm-source prerequisite holds it Shed even
	// though the hysteresis headroom is met, because the restart plus its
	// surge exceeds what firm generation can carry.
	{
		core::World world("RecC", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		// level 0.4 -> 8 kW (shed), level 0.6 -> 12 kW (above the 11 kW hysteresis threshold but below the 15 kW restart+surge).
		settings.solarArrays = { solar(20.0, stepProfile(100.0, {{0.0, 0.4}, {50.0, 0.6}})) };
		// heat-producing, dwell 0, hysteresis 0.1 (threshold 11), surge 5 -> restartNeed 15.
		settings.equipmentLoads = { recoveryLoad("kiln", 10.0, 0.0, 0.0, 0.1, 5.0, 1.0, 5.0) };
		configure(world, settings, "recovery C rig");
		world.resumeSimulation();
		runOrDiagnose(world, 3060, "recovery C"); // t = 51s
		auto const& l = loadSnap(world, "kiln");
		require(l.recoveryStage == core::StationLoadRecoveryStage::Shed,
			"the heat load restarted without enough firm generation");
		require(l.recoveryHoldReason.find("firm generation") != std::string::npos,
			"the heat load was not held by the firm-source prerequisite");
	}

	// Scenario D: a heat-producing load with a heat sink at or above its
	// maximum service temperature cannot restart - the cooling
	// qualification holds it Shed even with ample firm generation.
	{
		core::World world("RecD", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("hotSink", "low", 40.0, 40.0) }; // at its maximum service temp
		settings.solarArrays = { solar(20.0, stepProfile(100.0, {{0.0, 0.15}, {50.0, 0.6}})) };
		auto kiln = recoveryLoad("kiln2", 10.0, 0.0, 0.0, 0.0, 0.0, 0.0, 5.0);
		kiln.heatSinkId = "hotSink";
		settings.equipmentLoads = { kiln };
		configure(world, settings, "recovery D rig");
		world.resumeSimulation();
		runOrDiagnose(world, 3060, "recovery D"); // t = 51s
		auto const& l = loadSnap(world, "kiln2");
		require(l.recoveryStage == core::StationLoadRecoveryStage::Shed,
			"the heat load restarted onto a sink at or above its maximum service temperature");
		require(l.recoveryHoldReason.find("heat sink") != std::string::npos,
			"the heat load was not held by the cooling qualification");
	}

	// Scenario E: while a load is pending restart, the pending restart
	// headroom is held out of battery charging, so a depleted reserve
	// refills only from the surplus the recovery does not need and never
	// overloads the waiting restart.
	{
		core::World world("RecE", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		// level 0.25 -> 5 kW (shed), level 0.6 -> 12 kW (past dwell but below the 15 kW hysteresis threshold).
		settings.solarArrays = { solar(20.0, stepProfile(100.0, {{0.0, 0.25}, {50.0, 0.6}})) };
		settings.batteries = { battery("bank", 100.0, 80.0, 10.0) }; // depleted, wants to charge
		// demand 10, dwell 5, hysteresis 0.5 (threshold 15), no surge.
		settings.equipmentLoads = { recoveryLoad("waiter", 10.0, 0.0, 5.0, 0.5, 0.0, 0.0, 0.0) };
		configure(world, settings, "recovery E rig");
		world.resumeSimulation();
		runOrDiagnose(world, 3060, "recovery E"); // t = 51s
		auto const& snap = world.getStationSystemsSnapshot();
		require(loadSnap(world, "waiter").recoveryStage == core::StationLoadRecoveryStage::Shed,
			"the waiting load restarted although the pool was below its hysteresis threshold");
		require(snap.power.recoveryPending,
			"the recovery-pending flag did not raise while a load was Shed");
		// Surplus is 12 kW; the 10 kW pending restart headroom is held out,
		// so the battery charges at most 2 kW, not the full surplus.
		require(snap.power.batteries[0].lastChargeKW <= 2.0 + 1e-6,
			"battery charging was not capped by the pending restart headroom");
		require(snap.power.batteries[0].lastChargeKW > 0.0,
			"the depleted battery did not charge at all from the leftover surplus");
	}

	// Scenario F: the authored recovery rules survive the World document
	// round trip, and the reopened World reproduces the same recovery
	// behaviour tick-for-tick - persistence and reset stay compatible.
	{
		auto const path = context.temporaryRoot() / "recovery-roundtrip.world.yaml";
		core::World original("RecF", 2, 2);
		original.pauseSimulation(); original.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(20.0, stepProfile(100.0, {{0.0, 0.2}, {50.0, 0.75}})) };
		settings.equipmentLoads = { recoveryLoad("rig", 10.0, 0.0, 5.0, 0.2, 3.0, 1.5, 0.0) };
		configure(original, settings, "recovery F rig");
		original.resumeSimulation();
		runOrDiagnose(original, 3180, "recovery F original"); // t = 53s, surge done, recovered Active
		require(loadSnap(original, "rig").recoveryStage == core::StationLoadRecoveryStage::Active,
			"the recovery-F original did not reach the recovered state before saving");
		original.saveTo(path.string());
		auto const reopened = core::loadWorldDocument(path);
		require(reopened != nullptr, "the recovery document failed to reopen");
		require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
			"the authored recovery rules did not survive the document round trip");
		auto const reloaded = reopened->getStationSystemsSettings().equipmentLoads.at(0);
		require(closeTo(reloaded.recoveryDwellSeconds, 5.0)
			&& closeTo(reloaded.recoveryHysteresisFraction, 0.2)
			&& closeTo(reloaded.restartSurgePowerKW, 3.0)
			&& closeTo(reloaded.restartSurgeSeconds, 1.5),
			"the recovery settings did not round-trip with their declared values");
		reopened->resumeSimulation();
		runOrDiagnose(*reopened, 3180, "recovery F reopened"); // t = 53s
		require(loadSnap(*reopened, "rig").recoveryStage == core::StationLoadRecoveryStage::Active,
			"the reopened World did not reproduce the same recovery stage as the original");
	}

	// Scenario G (#588): a restart whose surge cannot be paid never runs
	// the equipment. A non-heat-producing 1 kW load with a required 1 kW
	// surge, zero dwell and zero hysteresis, and a battery that can
	// discharge nothing, sits behind exactly 1 kW of solar: the gate
	// releases it every tick because the pool covers its demand, but the
	// allocation pass refuses the restart because the surge is
	// unfundable. The load must never publish supplied service while it
	// is held Shed, must never consume a watt, and must never leave a
	// phantom battery discharge shortfall behind from a surge draw that
	// was never committed.
	{
		core::World world("RecG", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(1.0, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		settings.equipmentLoads = { recoveryLoad("starter", 1.0, 0.0, 0.0, 0.0, 1.0, 0.02, 0.0) };
		core::BatterySettings dead;
		dead.id = "dead"; dead.nameplateKWh = 1.0; dead.usableKWh = 1.0;
		dead.startingStoredKWh = 0.5; dead.maxDischargeKW = 0.0; dead.maxChargeKW = 0.0;
		settings.batteries = { dead };
		configure(world, settings, "recovery G rig");
		world.resumeSimulation();
		for (uint64_t tick = 1; tick <= 1500; ++tick) // t = 25s, past the release point
		{
			if (!world.advanceTick())
			{
				auto const diagnostics = world.getStationSystemsDiagnostics();
				throw std::runtime_error(std::string("recovery G tick failed: ")
					+ (diagnostics.empty() ? "(none)" : diagnostics.back().message));
			}
			auto const& l = loadSnap(world, "starter");
			require(!(l.suppliedKW > 1e-9
					&& l.recoveryStage == core::StationLoadRecoveryStage::Shed),
				"a failed restart published supplied service while returning to Shed");
			require(l.suppliedKW <= 1e-9,
				"an unfundable restart drew normal power it could not pair with its surge");
			require(l.recoverySurgeKW <= 1e-9,
				"an unfundable restart drew surge power");
			require(world.getStationSystemsSnapshot().power.batteries[0].lastDischargeShortfallKW
				<= 1e-9,
				"the refused surge left a battery discharge shortfall it never committed");
		}
	}

	// Scenario H (#588): a funded surge is real service. The whole
	// operable demand plus the surge is paid from generation and the
	// battery together, the supplied total counts the surge beside the
	// normal service, and the total surge energy never exceeds the
	// authored power x seconds budget - the last tick of a surge that
	// does not divide evenly by the timestep pays only what is left.
	{
		core::World world("RecH", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(3.0, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		// demand 2, min 2, dwell 0, hysteresis 0, surge 1 kW for 0.05s.
		settings.equipmentLoads = { recoveryLoad("pumper", 2.0, 0.0, 0.0, 0.0, 1.0, 0.05, 0.0) };
		configure(world, settings, "recovery H rig");
		world.resumeSimulation();
		double surgeEnergy = 0.0;
		double const authoredSurgeEnergy = 1.0 * 0.05;
		bool sawSurge = false;
		bool sawActive = false;
		for (uint64_t tick = 1; tick <= 1200; ++tick) // t = 20s
		{
			runOrDiagnose(world, 1, "recovery H");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "pumper");
			require(snap.power.totalSuppliedKW + 1e-9
				>= l.suppliedKW + l.recoverySurgeKW,
				"the supplied total did not count the startup surge");
			surgeEnergy += l.recoverySurgeKW * core::World::getFixedTimestep();
			if (l.recoverySurgeKW > 1e-9) sawSurge = true;
			if (l.recoveryStage == core::StationLoadRecoveryStage::Active) sawActive = true;
		}
		require(sawSurge, "the funded restart never drew its surge");
		require(sawActive, "the funded restart never settled to Active");
		require(surgeEnergy <= authoredSurgeEnergy + 1e-9,
			"the restart delivered more surge energy than was authored");
	}

	// Scenario I (#588): supply changes between the gate's release and
	// the restart's execution. The gate releases on a full pool; the
	// sun drops before the next tick allocates, and the allocation pass
	// refuses the restart before any service commits. The failed attempt
	// restarts the declared dwell: the load cannot try again until the
	// full dwell has elapsed behind the restored supply.
	{
		core::World world("RecI", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		// Step profile: dark until t=10, full sun at t=10 (the gate
		// releases), a collapse one tick later (the restart is refused
		// before execution), and restored sun at t=17 (restart once the
		// re-armed dwell has elapsed).
		settings.solarArrays = { solar(3.0, stepProfile(200.0,
			{{0.0, 0.0}, {9.999, 0.0}, {10.0, 1.0}, {10.001, 0.05},
				{17.0, 0.05}, {17.001, 1.0}})) };
		// demand 1, min 1, dwell 5, hysteresis 0, surge 1 kW for 0.5s.
		settings.equipmentLoads = { recoveryLoad("thwarted", 1.0, 0.0, 5.0, 0.0, 1.0, 0.5, 0.0) };
		configure(world, settings, "recovery I rig");
		world.resumeSimulation();
		runOrDiagnose(world, 600, "recovery I to release"); // t = 10s, gate releases
		require(loadSnap(world, "thwarted").recoveryStage == core::StationLoadRecoveryStage::Restoring,
			"the load was not released to Restoring on the full pool");
		runOrDiagnose(world, 1, "recovery I collapse"); // t = 10.0167s, supply collapsed
		{
			auto const& l = loadSnap(world, "thwarted");
			require(l.recoveryStage == core::StationLoadRecoveryStage::Shed,
				"the restart ran although the supply collapsed before execution");
			require(l.suppliedKW <= 1e-9,
				"the collapsed restart consumed operating power");
			require(l.recoverySurgeKW <= 1e-9,
				"the collapsed restart drew surge power");
			// The failed attempt re-armed the full five-second dwell.
			require(l.recoverySecondsRemaining > 4.9,
				"the failed restart did not restart the declared dwell");
		}
		runOrDiagnose(world, 600, "recovery I restored"); // t = 20s, sun restored, dwell elapsed
		require(loadSnap(world, "thwarted").recoveryStage == core::StationLoadRecoveryStage::Active,
			"the load did not restart once the supply and the full dwell were behind it");
	}

	// Scenario J (#588): a funded restart that cannot have its full
	// normal demand keeps the surge whole. The claim is the normal demand
	// plus the surge; the surge is a hard requirement, so when the pool
	// is tight the normal service is curtailed to its minimum operable
	// level and the surge still draws in full - the two are funded as
	// one unit and never exceed the pool.
	{
		core::World world("RecJ", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(5.0, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		// demand 4, maxCurtailment 0.5 (min 2), dwell 0, hysteresis 0,
		// surge 2 kW for 0.05s. The pool of 5 kW funds normal 3 + surge 2.
		settings.equipmentLoads = { recoveryLoad("drill", 4.0, 0.5, 0.0, 0.0, 2.0, 0.05, 0.0) };
		configure(world, settings, "recovery J rig");
		world.resumeSimulation();
		double surgeEnergy = 0.0;
		bool sawCurtailedSurge = false;
		bool sawActive = false;
		for (uint64_t tick = 1; tick <= 480; ++tick) // t = 8s
		{
			runOrDiagnose(world, 1, "recovery J");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "drill");
			require(l.suppliedKW + l.recoverySurgeKW <= 5.0 + 1e-9,
				"the restart claim drew more than the pool could carry");
			require(snap.power.totalSuppliedKW + 1e-9
				>= l.suppliedKW + l.recoverySurgeKW,
				"the supplied total did not count the startup surge");
			if (l.recoverySurgeKW > 1e-9)
			{
				require(l.suppliedKW >= 2.0 - 1e-9,
					"the funded restart curtailed normal service below its minimum operable level");
				require(l.suppliedKW <= 3.0 + 1e-6,
					"the funded restart took normal service the surge still needed");
				sawCurtailedSurge = true;
			}
			surgeEnergy += l.recoverySurgeKW * core::World::getFixedTimestep();
			if (l.recoveryStage == core::StationLoadRecoveryStage::Active) sawActive = true;
		}
		require(sawCurtailedSurge, "the restart never ran with a curtailed normal and a full surge");
		require(sawActive, "the restart never settled to Active");
		require(surgeEnergy <= 2.0 * 0.05 + 1e-9,
			"the restart delivered more surge energy than was authored");
	}

	// Scenario K (#591): a sub-tick surge is prorated, not rounded up to
	// full ticks, and the battery funding it loses only the authored
	// energy. A 1 kW surge authored for 0.02 s is barely longer than one
	// fixed tick (1/60 s): the old code always bought the full 1 kW and
	// only then subtracted a full dt, drawing 0.0333 kJ against the
	// 0.02 kJ contract - a 66.7% overdraw the battery actually paid,
	// and far worse below a tick. The last tick of a surge now pays only
	// the energy left in the declared budget: the surge power is
	// prorated by window/tick, the total delivered energy never exceeds
	// power x seconds, and the battery's discharge ledger matches it
	// exactly. A 0.01 s surge - shorter than one tick - must be paid in
	// a single prorated tick, and every trace replays identically after
	// a reset.
	{
		struct SurgeRun
		{
			std::vector<double> trace;
			double energy{0.0};
			int surgeTicks{0};
		};
		auto runSurgeRig = [&](core::World& world, double surgeSeconds)
		{
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.stores = { massStore("ballast", 10.0, 1.0) };
			// 2 kW of solar covers the load's own 2 kW demand exactly once
			// the sun rises at t=5; the startup surge on top of it has to
			// come out of the battery.
			settings.solarArrays = { solar(2.0, stepProfile(200.0, {{0.0, 0.0}, {5.0, 1.0}})) };
			settings.equipmentLoads = { recoveryLoad("surger", 2.0, 0.0, 0.0, 0.0, 1.0, surgeSeconds, 0.0) };
			core::BatterySettings cell;
			cell.id = "cell"; cell.nameplateKWh = 1.0; cell.usableKWh = 1.0;
			cell.startingStoredKWh = 0.5; cell.maxDischargeKW = 1.0; cell.maxChargeKW = 0.0;
			settings.batteries = { cell };
			configure(world, settings, "recovery K rig");
			world.resumeSimulation();
		};
		auto driveSurge = [&](core::World& world, int ticks)
		{
			SurgeRun run;
			for (int tick = 1; tick <= ticks; ++tick)
			{
				runOrDiagnose(world, 1, "recovery K");
				auto const& l = loadSnap(world, "surger");
				run.trace.push_back(l.recoverySurgeKW);
				run.energy += l.recoverySurgeKW * core::World::getFixedTimestep();
				if (l.recoverySurgeKW > 1e-9) run.surgeTicks += 1;
			}
			return run;
		};
		// The battery funded the whole surge and nothing else: the energy
		// it lost from storage equals the surge energy delivered to the
		// load, so no overdraw is ever paid by the reserve.
		auto checkBattery = [&](core::World& world, double surgeEnergy, char const* what)
		{
			auto const snap = world.getStationSystemsSnapshot();
			auto const& bat = snap.power.batteries[0];
			require(closeTo(bat.dischargedKWh, surgeEnergy / 3600.0, 1e-9),
				(std::string(what) + ": the battery lost energy the surge never delivered").c_str());
			require(closeTo(bat.storedKWh, 0.5 - surgeEnergy / 3600.0, 1e-9),
				(std::string(what) + ": the battery's remaining charge does not match the surge draw").c_str());
		};
		auto checkReplay = [&](SurgeRun const& first, SurgeRun const& replay)
		{
			require(first.trace.size() == replay.trace.size(),
				"the surge replay trace length changed after reset");
			for (size_t i = 0; i < first.trace.size() && i < replay.trace.size(); ++i)
				require(closeTo(first.trace[i], replay.trace[i], 1e-12),
					"the surge proration is not deterministic across a reset/replay");
			require(closeTo(first.energy, replay.energy, 1e-12),
				"the surge energy is not deterministic across a reset/replay");
		};
		// Sub-tick surge: a 0.01 s budget is shorter than one tick and
		// is paid whole in a single prorated tick, never as a full-tick
		// purchase.
		{
			core::World world("RecK-short", 2, 2);
			runSurgeRig(world, 0.01);
			auto const first = driveSurge(world, 330); // t = 5.5s, past release + full surge
			require(first.surgeTicks == 1,
				"a sub-tick surge was not paid in a single prorated tick");
			require(closeTo(first.energy, 1.0 * 0.01, 1e-6),
				"the sub-tick surge delivered a different energy than was authored");
			for (auto const kw : first.trace)
				if (kw > 1e-9)
					require(closeTo(kw, 1.0 * 0.01 / core::World::getFixedTimestep(), 1e-6),
						"the sub-tick surge was not prorated to the remaining window");
			require(loadSnap(world, "surger").recoveryStage == core::StationLoadRecoveryStage::Active,
				"the sub-tick surge did not settle the load to Active");
			checkBattery(world, first.energy, "sub-tick surge");
			world.resetSimulation(); world.resumeSimulation();
			auto const replay = driveSurge(world, 330);
			checkReplay(first, replay);
			checkBattery(world, replay.energy, "sub-tick surge replay");
		}
		// Ticket-shaped partial tail: a 0.02 s surge is one full tick at
		// the declared 1 kW plus one partial tick at the prorated
		// remainder - never a second full-tick purchase.
		{
			core::World world("RecK-ticket", 2, 2);
			runSurgeRig(world, 0.02);
			auto const first = driveSurge(world, 330);
			require(first.surgeTicks == 2,
				"a 0.02 s surge did not draw exactly one full tick and one partial tick");
			require(closeTo(first.energy, 1.0 * 0.02, 1e-6),
				"the partial-tick surge delivered a different energy than was authored");
			std::vector<double> drawn;
			for (auto const kw : first.trace)
				if (kw > 1e-9) drawn.push_back(kw);
			require(drawn.size() == 2
				&& closeTo(drawn[0], 1.0, 1e-6)
				&& closeTo(drawn[1], 0.2, 1e-6),
				"the partial final tick of the surge was not prorated to the remainder");
			checkBattery(world, first.energy, "ticket-shaped surge");
			world.resetSimulation(); world.resumeSimulation();
			auto const replay = driveSurge(world, 330);
			checkReplay(first, replay);
			checkBattery(world, replay.energy, "ticket-shaped surge replay");
		}
		// Multi-tick partial tail: a 0.055 s surge is three full ticks at
		// 1 kW plus one partial tick at the prorated remainder.
		{
			core::World world("RecK-partial", 2, 2);
			runSurgeRig(world, 0.055);
			auto const first = driveSurge(world, 330);
			require(first.surgeTicks == 4,
				"a 0.055 s surge did not draw three full ticks and one partial tick");
			require(closeTo(first.energy, 1.0 * 0.055, 1e-6),
				"the partial-tick surge delivered a different energy than was authored");
			std::vector<double> drawn;
			for (auto const kw : first.trace)
				if (kw > 1e-9) drawn.push_back(kw);
			require(drawn.size() == 4
				&& closeTo(drawn[0], 1.0, 1e-6)
				&& closeTo(drawn[1], 1.0, 1e-6)
				&& closeTo(drawn[2], 1.0, 1e-6)
				&& closeTo(drawn[3], 0.3, 1e-6),
				"the partial final tick of the surge was not prorated to the remainder");
			checkBattery(world, first.energy, "multi-tick partial-tail surge");
			world.resetSimulation(); world.resumeSimulation();
			auto const replay = driveSurge(world, 330);
			checkReplay(first, replay);
			checkBattery(world, replay.energy, "multi-tick partial-tail surge replay");
		}
	}

	// Shared rig for the #589 scenarios: no solar, no battery, one
	// fuelled generator with zero startup time and power behind a cool
	// sink, and automatic dispatch. The only thing that can put power on
	// the bus is a generator the dispatcher chose to start.
	struct DispatchRig
	{
		core::StationSystemsSettings settings;
	};
	auto makeDispatchRig = [&]()
	{
		DispatchRig rig;
		auto& settings = rig.settings;
		core::ResourceStoreSettings fuel;
		fuel.id = "fuel"; fuel.unit = "kWh";
		fuel.capacity = 100.0; fuel.startingQualified = 100.0;
		settings.stores = { fuel };
		settings.dispatchMode = core::StationDispatchMode::Automatic;
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0, 200.0) };
		core::GeneratorSettings gen;
		gen.id = "gen"; gen.fuelStoreId = "fuel"; gen.thermalClass = "low";
		gen.heatSinkId = "sink"; gen.ratedOutputKW = 10.0; gen.efficiency = 0.5;
		gen.maxRampKWPerSecond = 1000.0; gen.startupPowerKW = 0.0;
		gen.startupSeconds = 0.0; gen.maxSinkTemperatureC = 90.0;
		settings.generators = { gen };
		return rig;
	};
	auto storeSnap = [](core::World& world, std::string const& id) -> core::StationSystemsStoreSnapshot
	{
		for (auto const& s : world.getStationSystemsSnapshot().stores)
			if (s.id == id) return s;
		throw std::runtime_error("store not found: " + id);
	};

	// Scenario K (#589): the ticket's deadlock. With no solar and no
	// battery, a shed load drove no dispatch demand, so the dispatcher
	// never commanded the generator, the spare pool never appeared, and
	// the load stayed Shed forever with fuel still in the tank - a
	// circular wait between the restart gate and the dispatcher. Now
	// the shed load's pending restart feeds the dispatch signal, the
	// generator comes online to fund the restart, and the load returns
	// to full service.
	{
		core::World world("RecK", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto rig = makeDispatchRig();
		rig.settings.equipmentLoads = { recoveryLoad("rig", 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0) };
		configure(world, rig.settings, "recovery K rig");
		world.resumeSimulation();
		bool sawShed = false;
		bool sawPendingDemand = false;
		for (uint64_t tick = 1; tick <= 120; ++tick) // t = 2s
		{
			runOrDiagnose(world, 1, "recovery K");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "rig");
			if (l.recoveryStage == core::StationLoadRecoveryStage::Shed)
			{
				sawShed = true;
				if (snap.power.dispatchDemandKW > 1e-9) sawPendingDemand = true;
			}
		}
		require(sawShed, "the load never shed against the empty pool");
		require(sawPendingDemand,
			"the shed load's pending restart never reached the dispatch signal");
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& l = loadSnap(world, "rig");
		require(l.recoveryStage == core::StationLoadRecoveryStage::Active,
			"the load never restarted under automatic dispatch");
		require(closeTo(l.suppliedKW, 1.0), "the restarted load is not fully supplied");
		require(snap.power.generators[0].outputKW >= 1.0 - 1e-9,
			"the generator never came online for the waiting restart");
		require(storeSnap(world, "fuel").currentQualified < 100.0,
			"the generator served the load without drawing fuel");
	}

	// Scenario L (#589): the surge of a restart in flight keeps the
	// source online. A load with a declared startup surge restarted by
	// automatic dispatch must be able to pay its whole surge budget
	// without the dispatch signal dropping mid-restart and sending the
	// load back to Shed - the surge rides the signal until it is paid,
	// then the station settles back to the load's normal demand.
	{
		core::World world("RecL", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto rig = makeDispatchRig();
		// demand 1, min 1, dwell 0, hysteresis 0, surge 1 kW for 0.5s.
		rig.settings.equipmentLoads = { recoveryLoad("driller", 1.0, 0.0, 0.0, 0.0, 1.0, 0.5, 0.0) };
		configure(world, rig.settings, "recovery L rig");
		world.resumeSimulation();
		double surgeEnergy = 0.0;
		bool sawSurge = false;
		for (uint64_t tick = 1; tick <= 1200; ++tick) // t = 20s
		{
			runOrDiagnose(world, 1, "recovery L");
			auto const& l = loadSnap(world, "driller");
			require(!(l.recoveryStage == core::StationLoadRecoveryStage::Shed
					&& l.suppliedKW > 1e-9),
				"a failed restart published supplied service while returning to Shed");
			if (l.recoverySurgeKW > 1e-9) sawSurge = true;
			surgeEnergy += l.recoverySurgeKW * core::World::getFixedTimestep();
		}
		require(sawSurge, "the automatic-dispatch restart never drew its surge");
		auto const& l = loadSnap(world, "driller");
		require(l.recoveryStage == core::StationLoadRecoveryStage::Active,
			"the surge restart never settled to Active under automatic dispatch");
		require(closeTo(l.suppliedKW, 1.0), "the settled load is not fully supplied");
		require(surgeEnergy <= 1.0 * 0.5 + 1e-9,
			"the restart delivered more surge energy than was authored");
	}

	// Scenario M (#589): a shed load whose declared feed cannot sustain
	// it never drives fuel burn. Same rig as K, but the load consumes
	// from an empty store: the restart is barred by the feed itself,
	// so its need never reaches the dispatcher, the generator stays
	// stopped, and not one unit of fuel is burned for equipment that
	// cannot run.
	{
		core::World world("RecM", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto rig = makeDispatchRig();
		rig.settings.stores.push_back(massStore("brine", 10.0, 0.0));
		auto starved = recoveryLoad("starved", 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
		core::EquipmentMaterialExchangeSettings feed;
		feed.storeId = "brine"; feed.ratePerSecond = 0.01; feed.consumes = true;
		starved.materialExchanges = { feed };
		rig.settings.equipmentLoads = { starved };
		configure(world, rig.settings, "recovery M rig");
		world.resumeSimulation();
		runOrDiagnose(world, 120, "recovery M"); // t = 2s
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& l = loadSnap(world, "starved");
		require(l.recoveryStage == core::StationLoadRecoveryStage::Shed,
			"a feed-starved load restarted despite its infeasible feed");
		require(l.suppliedKW <= 1e-9, "a shed load drew power");
		require(snap.power.dispatchDemandKW <= 1e-9,
			"a feed-starved shed load drove the dispatch signal");
		require(snap.power.generators[0].outputKW <= 1e-9,
			"the generator ran for equipment its feed cannot sustain");
		require(closeTo(storeSnap(world, "fuel").currentQualified, 100.0),
			"fuel was burned for a load that cannot run on its declared feed");
	}

	// Scenario N (#590): a battery-funded surge is booked through the
	// same station-wide aggregate accounting as every other consumer.
	// A 2 kW load behind 2 kW of sun can pay its normal service from
	// generation but nothing of its 1 kW surge: the whole surge is
	// battery delivery. The station-wide discharge must show that
	// surge exactly once, the station-wide shortfall must agree with
	// the single bank's own shortfall on every tick (the ticket's bug
	// hid a bank shortfall behind a zero aggregate), and the bank's
	// energy ledger must close over the surge with no phantom draw.
	// The rig is then reset and replayed to prove the battery surge
	// state is deterministic.
	{
		core::World world("RecN", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(2.0, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		// demand 2, min 2, dwell 0, hysteresis 0, surge 1 kW for 0.05s.
		settings.equipmentLoads = { recoveryLoad("surger", 2.0, 0.0, 0.0, 0.0, 1.0, 0.05, 0.0) };
		core::BatterySettings bank;
		bank.id = "bank"; bank.nameplateKWh = 1.0; bank.usableKWh = 1.0;
		bank.startingStoredKWh = 0.5; bank.maxDischargeKW = 1.0; bank.maxChargeKW = 0.0;
		settings.batteries = { bank };
		configure(world, settings, "recovery N rig");
		world.resumeSimulation();
		double surgeEnergy = 0.0;
		bool sawBatterySurge = false;
		for (uint64_t tick = 1; tick <= 480; ++tick) // t = 8s, past the restart
		{
			runOrDiagnose(world, 1, "recovery N");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "surger");
			// One bank: the station-wide shortfall is exactly the bank's,
			// every tick, funded or not.
			require(std::abs(snap.power.batteryShortfallKW
					- snap.power.batteries[0].lastDischargeShortfallKW) <= 1e-9,
				"the station-wide battery shortfall disagreed with the bank's surge shortfall");
			require(snap.power.totalSuppliedKW + 1e-9
				>= l.suppliedKW + l.recoverySurgeKW,
				"the supplied total did not count the battery-funded surge");
			if (l.recoverySurgeKW > 1e-9)
			{
				// Generation is fully spent on the normal service, so the
				// whole surge is battery delivery and shows in the
				// station-wide discharge exactly once.
				require(std::abs(snap.power.batteryDischargeKW - l.recoverySurgeKW) <= 1e-9,
					"the battery-funded surge was not booked into the station-wide discharge");
				require(l.suppliedKW >= 2.0 - 1e-9,
					"the battery-funded surge starved the load's normal service");
				sawBatterySurge = true;
			}
			surgeEnergy += l.recoverySurgeKW * core::World::getFixedTimestep();
		}
		require(sawBatterySurge, "the surge was never battery-funded");
		require(closeTo(surgeEnergy, 1.0 * 0.05, 1e-9),
			"the battery-funded surge did not deliver its authored energy");
		{
			auto const& snap = world.getStationSystemsSnapshot();
			auto const surgeEnergyKWh = surgeEnergy / 3600.0;
			require(closeTo(snap.power.batteries[0].deliveredKWh, surgeEnergyKWh, 1e-9),
				"the bank's delivered ledger does not match the surge energy booked");
			require(closeTo(snap.power.batteries[0].storedKWh, 0.5 - surgeEnergyKWh, 1e-9),
				"the bank's stored energy does not close over the surge with no phantom draw");
		}
		// Deterministic reset/replay (#590): the same authored rig
		// replayed from a reset reproduces the battery surge ledger
		// exactly - host and bundled model agree tick for tick.
		auto const before = world.getStationSystemsSnapshot();
		world.pauseSimulation(); world.resetSimulation(); world.resumeSimulation();
		runOrDiagnose(world, 480, "recovery N replay");
		require(world.getStationSystemsSnapshot() == before,
			"the battery-funded surge did not replay deterministically after reset");
	}

	// Scenario O (#590): a surge paid partly by generation and partly
	// by the battery. 2.5 kW of sun covers the 2 kW normal service and
	// half of the 1 kW surge; the battery delivers the other half. The
	// split battery delivery is booked into the station-wide discharge
	// and the shortfall keeps agreeing with the single bank - now at a
	// nonzero delivery with zero unserved remainder.
	{
		core::World world("RecO", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(2.5, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		// demand 2, min 2, dwell 0, hysteresis 0, surge 1 kW for 0.05s.
		settings.equipmentLoads = { recoveryLoad("mixer", 2.0, 0.0, 0.0, 0.0, 1.0, 0.05, 0.0) };
		core::BatterySettings bank;
		bank.id = "bank"; bank.nameplateKWh = 1.0; bank.usableKWh = 1.0;
		bank.startingStoredKWh = 0.5; bank.maxDischargeKW = 1.0; bank.maxChargeKW = 0.0;
		settings.batteries = { bank };
		configure(world, settings, "recovery O rig");
		world.resumeSimulation();
		bool sawSplitSurge = false;
		for (uint64_t tick = 1; tick <= 480; ++tick) // t = 8s, past the restart
		{
			runOrDiagnose(world, 1, "recovery O");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "mixer");
			require(std::abs(snap.power.batteryShortfallKW
					- snap.power.batteries[0].lastDischargeShortfallKW) <= 1e-9,
				"the station-wide battery shortfall disagreed with the bank's during the split surge");
			require(snap.power.totalSuppliedKW + 1e-9
				>= l.suppliedKW + l.recoverySurgeKW,
				"the supplied total did not count the split-funded surge");
			if (l.recoverySurgeKW > 0.9)
			{
				// Full-surge ticks: generation carries 0.5 kW of the surge
				// and the battery the other half, exactly once. The last,
				// prorated tick of the surge budget carries the same split
				// against its shortened slice.
				auto const expectedBatteryShare = std::max(0.0, l.recoverySurgeKW - 0.5);
				require(std::abs(snap.power.batteryDischargeKW - expectedBatteryShare) <= 1e-6,
					"the battery's half of the split surge was not booked exactly once");
				require(std::abs(l.suppliedKW - 2.0) <= 1e-9,
					"the split surge disturbed the load's normal service");
				sawSplitSurge = true;
			}
			else if (l.recoverySurgeKW > 1e-9)
			{
				// The prorated tail fits inside generation's leftover, so
				// the battery never delivers more than its half-share.
				auto const expectedBatteryShare = std::max(0.0, l.recoverySurgeKW - 0.5);
				require(std::abs(snap.power.batteryDischargeKW - expectedBatteryShare) <= 1e-6,
					"the battery delivered beyond its share of the surge tail");
			}
		}
		require(sawSplitSurge, "the surge never ran split between generation and battery");
	}

	// Scenario P (#590): multiple banks route one surge in declaration
	// order and the station-wide totals count every watt exactly once.
	// The banks together carry only 1.5 kW - below the 2 kW minimum -
	// so the dark half sheds the load, and the restart surge of 1 kW
	// routes across them: the small bank can take only 0.4 kW and the
	// big bank carries the rest. The station-wide discharge is the sum
	// of the two banks' delivery, and the station-wide shortfall stays
	// zero: the small bank's routed-but-later-served remainder is its
	// own per-bank diagnostic and never leaks into the station
	// shortage readout as a double count.
	{
		core::World world("RecP", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(2.0, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		// demand 2, min 2, dwell 0, hysteresis 0, surge 1 kW for 0.05s.
		settings.equipmentLoads = { recoveryLoad("rig", 2.0, 0.0, 0.0, 0.0, 1.0, 0.05, 0.0) };
		core::BatterySettings small;
		small.id = "small"; small.nameplateKWh = 1.0; small.usableKWh = 1.0;
		small.startingStoredKWh = 0.5; small.maxDischargeKW = 0.4; small.maxChargeKW = 0.0;
		core::BatterySettings big;
		big.id = "big"; big.nameplateKWh = 1.0; big.usableKWh = 1.0;
		big.startingStoredKWh = 0.5; big.maxDischargeKW = 1.1; big.maxChargeKW = 0.0;
		settings.batteries = { small, big };
		configure(world, settings, "recovery P rig");
		world.resumeSimulation();
		bool sawMultiBankSurge = false;
		for (uint64_t tick = 1; tick <= 480; ++tick) // t = 8s, past the restart
		{
			runOrDiagnose(world, 1, "recovery P");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "rig");
			require(snap.power.totalSuppliedKW + 1e-9
				>= l.suppliedKW + l.recoverySurgeKW,
				"the supplied total did not count the multi-bank surge");
			if (l.recoverySurgeKW > 1e-9)
			{
				// The whole surge is battery delivery and the station-wide
				// discharge is the two banks' delivery counted once.
				require(std::abs(snap.power.batteryDischargeKW - l.recoverySurgeKW) <= 1e-9,
					"the multi-bank surge was not booked into the station-wide discharge exactly once");
				require(std::abs(snap.power.batteryDischargeKW
						- (snap.power.batteries[0].lastDischargeKW
							+ snap.power.batteries[1].lastDischargeKW)) <= 1e-9,
					"the station-wide discharge is not the sum of the banks' delivery");
				// Declaration-order routing: the small bank takes the surge
				// first, up to its power limit.
				auto const expectedSmall = std::min(l.recoverySurgeKW, 0.4);
				require(std::abs(snap.power.batteries[0].lastDischargeKW - expectedSmall) <= 1e-9,
					"the surge did not route through the banks in declaration order");
				// Nothing went unserved, so the station-wide shortfall is
				// zero - it never accumulates the small bank's routed
				// remainder that the big bank went on to deliver.
				require(snap.power.batteryShortfallKW <= 1e-9,
					"a served routed remainder double-counted into the station battery shortfall");
				sawMultiBankSurge = true;
			}
		}
		require(sawMultiBankSurge, "the surge never routed across multiple banks");
	}

	// Scenario Q (#590): the ticket's probe through the public seam. A
	// 1 kW load with a 1 kW surge behind exactly 1 kW of sun and a
	// battery that can discharge nothing: the restart is unfundable
	// every tick. Nothing is routed to the battery and nothing is
	// refused, so the station-wide shortfall and the bank's own
	// shortfall agree at zero - the aggregate can never hide a
	// routed-but-unserved surge from the station readout, and the
	// refused restart never publishes service.
	{
		core::World world("RecQ", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(1.0, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		// demand 1, min 1, dwell 0, hysteresis 0, surge 1 kW for 0.02s.
		settings.equipmentLoads = { recoveryLoad("starter", 1.0, 0.0, 0.0, 0.0, 1.0, 0.02, 0.0) };
		core::BatterySettings dead;
		dead.id = "dead"; dead.nameplateKWh = 1.0; dead.usableKWh = 1.0;
		dead.startingStoredKWh = 0.5; dead.maxDischargeKW = 0.0; dead.maxChargeKW = 0.0;
		settings.batteries = { dead };
		configure(world, settings, "recovery Q rig");
		world.resumeSimulation();
		for (uint64_t tick = 1; tick <= 600; ++tick) // t = 10s of refused restarts
		{
			runOrDiagnose(world, 1, "recovery Q");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "starter");
			require(std::abs(snap.power.batteryShortfallKW
					- snap.power.batteries[0].lastDischargeShortfallKW) <= 1e-9,
				"the station-wide battery shortfall hid the bank's shortfall on the failed surge");
			require(l.suppliedKW <= 1e-9, "the unfundable restart published normal service");
			require(l.recoverySurgeKW <= 1e-9, "the unfundable restart drew surge power");
			require(snap.power.batteryDischargeKW <= 1e-9,
				"the failed surge discharged the battery it could not pay with");
		}
		require(loadSnap(world, "starter").recoveryStage != core::StationLoadRecoveryStage::Active,
			"an unfundable surge restart settled to Active without ever paying its surge");
	}
}
}

void registerStationResilienceChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"dispatch", dispatch});
	checks.push_back({"protectedServices", protectedServices});
	checks.push_back({"recovery", recovery});
}
