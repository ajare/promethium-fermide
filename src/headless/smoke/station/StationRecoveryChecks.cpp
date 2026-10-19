#include "StationRecoveryChecks.h"
#include "StationRig.h"
#include <limits>

namespace
{
	using namespace smoke::station;

	// Carbon recovery through Sabatier and methane processing (#405): the
	// reactor's conversion closes on mass (CO2 + hydrogen = methane +
	// water) and on energy (supplied + released = routed), the processor's
	// split closes on mass (methane = hydrogen + carbon) and on energy
	// (supplied = absorbed + routed), the combined chain retains a net
	// hydrogen demand because the reactor consumes twice the hydrogen per
	// methane as the processor returns, and blocked destinations, hot
	// sinks, missing power and disabled stages all refuse or cap without
	// gifting product.
	void carbonRecovery(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto const h2PerCo2 = core::StationSabatierHydrogenMassPerCarbonDioxideKg;
		auto const ch4PerCo2 = core::StationSabatierMethaneMassPerCarbonDioxideKg;
		auto const h2oPerCo2 = core::StationSabatierWaterMassPerCarbonDioxideKg;
		auto const released = core::StationSabatierReactionHeatKJPerKgCarbonDioxide;
		auto const h2PerCh4 = core::StationMethaneProcessingHydrogenMassPerMethaneKg;
		auto const cPerCh4 = core::StationMethaneProcessingCarbonMassPerMethaneKg;
		auto const demand = core::StationMethaneProcessingReactionHeatKJPerKgMethane;
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::abs(expected);
		};
		// The declared stoichiometry must be exothermic on the Sabatier
		// side and endothermic on the pyrolysis side, and the hydrogen
		// returned per methane must be exactly half the hydrogen consumed
		// per CO2 that made that methane - the net-demand identity the
		// combined chain relies on.
		require(released > 0.0, "the declared Sabatier reaction is not exothermic");
		require(demand > 0.0, "the declared methane-processing reaction is not endothermic");
		require(approx(ch4PerCo2 * h2PerCh4, h2PerCo2 / 2.0),
			"the recycled hydrogen is not exactly half the Sabatier hydrogen demand");
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
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = 10000.0;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto reactor = [](std::string const& id, std::string const& hazard)
		{
			core::SabatierReactorSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.co2StoreId = "co2";
			settings.hydrogenStoreId = "h2";
			settings.methaneStoreId = "methane";
			settings.waterStoreId = "rawWater";
			settings.thermalClass = "low";
			settings.heatSinkId = "sink";
			settings.rateKgPerS = 0.001;
			settings.specificEnergyKWhPerKgCO2 = 1.0;
			settings.maxSinkTemperatureC = 60.0;
			return settings;
		};
		auto processor = [](std::string const& id, std::string const& hazard)
		{
			core::MethaneProcessorSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.methaneStoreId = "methane";
			settings.hydrogenStoreId = "h2";
			settings.carbonStoreId = "carbon";
			settings.thermalClass = "low";
			settings.heatSinkId = "sink";
			settings.rateKgPerS = 0.001;
			settings.specificEnergyKWhPerKgCH4 = 2.0;
			settings.maxSinkTemperatureC = 60.0;
			return settings;
		};
		auto carbonStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("co2", 100.0, 10.0));
			stores.push_back(plainStore("h2", 100.0, 10.0));
			stores.push_back(plainStore("methane", 100.0, 0.0));
			stores.push_back(plainStore("rawWater", 100.0, 0.0));
			stores.push_back(plainStore("carbon", 100.0, 0.0));
			return stores;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto carbonConfigure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::SabatierReactorSettings> reactors,
			std::vector<core::MethaneProcessorSettings> processors,
			double solarKW,
			double sinkStartingC = 10.0)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.thermalReservoirs = { reservoir("sink", "low", sinkStartingC) };
			settings.sabatierReactors = std::move(reactors);
			settings.methaneProcessors = std::move(processors);
			if (solarKW > 0.0)
			{
				core::SolarArraySettings field;
				field.id = "field";
				field.capacityKW = solarKW;
				settings.solarArrays = { field };
			}
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed carbon-recovery contract
		// is rejected before anything runs, and none of the refusals
		// mutates the World.
		{
			core::World bad("CarbonBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = carbonStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			std::string diagnostic;
			settings.sabatierReactors = { reactor("sab", "normal") };
			settings.sabatierReactors[0].co2StoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("CO2 feed store 'ghost' does not exist") != std::string::npos,
				"a Sabatier reactor fed by an unknown CO2 store was accepted");
			settings.sabatierReactors = { reactor("sab", "normal") };
			settings.sabatierReactors[0].heatSinkId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown thermal reservoir") != std::string::npos,
				"a Sabatier reactor on an unknown heat sink was accepted");
			settings.sabatierReactors = { reactor("sab", "normal") };
			settings.sabatierReactors[0].thermalClass = "warm";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("does not match heat sink") != std::string::npos,
				"a thermal class mismatch against the reactor's heat sink was accepted");
			settings.sabatierReactors = { reactor("sab", "normal"), reactor("sab2", "hazardous") };
			// A hazardous reactor sharing the normal-class CO2 store is
			// accepted (#751): the class no longer reserves a store, and
			// the shared inventory is the consequence.
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"a hazardous reactor sharing the normal-class CO2 store was refused");
			settings.sabatierReactors = { reactor("sab", "normal") };
			settings.sabatierReactors[0].maxSinkTemperatureC = -300.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"a reactor sink temperature below absolute zero was accepted");
			settings.sabatierReactors = {};
			settings.methaneProcessors = { processor("pyrolysis", "normal") };
			settings.methaneProcessors[0].specificEnergyKWhPerKgCH4 = 1.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("below the endothermic reaction heat") != std::string::npos,
				"a methane dose below the declared reaction demand was accepted");
			settings.methaneProcessors = { processor("pyrolysis", "normal") };
			settings.methaneProcessors[0].carbonStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("solid carbon store 'ghost' does not exist") != std::string::npos,
				"a processor with an unknown carbon store was accepted");
			settings.methaneProcessors = { processor("pyrolysis", "normal"), processor("pyrolysis2", "hazardous") };
			// A hazardous processor sharing the normal-class methane store is
			// accepted (#751): the shared inventory is the consequence.
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"a hazardous processor sharing the normal-class methane store was refused");
			std::vector<core::SabatierReactorSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationSabatierReactors; ++index)
				tooMany.push_back(reactor("sab" + std::to_string(index), "normal"));
			settings = core::StationSystemsSettings{};
			settings.enabled = true;
			settings.stores = carbonStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.sabatierReactors = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared Sabatier reactor budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused carbon-recovery configuration mutated the World");
		}
		// Aliased stream roles (#599), the same rule #585 carries for the
		// electrolyzer and the fuel cell: every pair of chemically distinct
		// roles on one machine must name a distinct store. The reported
		// reproduction named one nearly-full store for both the methane and
		// the water product: each product was sized against the same 0.01 kg
		// of headroom, both were booked, and the finite-capacity audit
		// failed on the very first tick - accepted authored configuration
		// turning a finite physical limit into a terminal failure that
		// paused the World at tick zero. Every pair of roles is refused at
		// configuration, with nearly full products and nearly empty feeds,
		// and the World is never mutated.
		{
			auto const alias = "chemically distinct streams may not share one store";
			struct Alias
			{
				std::string left;
				std::string right;
			};
			// Nearly full products, nearly empty feeds: the refusal is about
			// the roles, not the fill levels, so it must fire at any level.
			auto tightStores = [&]()
			{
				std::vector<core::ResourceStoreSettings> stores;
				stores.push_back(plainStore("co2", 100.0, 0.001));
				stores.push_back(plainStore("h2", 100.0, 0.001));
				stores.push_back(plainStore("methane", 1.0, 0.99));
				stores.push_back(plainStore("rawWater", 1.0, 0.99));
				stores.push_back(plainStore("carbon", 1.0, 0.99));
				return stores;
			}();
			core::World tight("CarbonAliasedRoles", 2, 2);
			tight.pauseSimulation(); tight.finishBuild();
			std::string diagnostic;
			// The role names below are the fixture store ids themselves, so
			// aliasing a pair means pointing the right-hand role at the
			// left-hand role's store.
			auto setReactorRole = [](core::SabatierReactorSettings& unit,
				std::string const& role, std::string const& id)
			{
				if (role == "co2") unit.co2StoreId = id;
				else if (role == "h2") unit.hydrogenStoreId = id;
				else if (role == "methane") unit.methaneStoreId = id;
				else unit.waterStoreId = id;
			};
			auto setProcessorRole = [](core::MethaneProcessorSettings& unit,
				std::string const& role, std::string const& id)
			{
				if (role == "methane") unit.methaneStoreId = id;
				else if (role == "h2") unit.hydrogenStoreId = id;
				else unit.carbonStoreId = id;
			};
			// Sabatier reactor: all six pairs of its four stream roles.
			std::vector<Alias> reactorAliases = {
				{"co2", "h2"}, {"co2", "methane"}, {"co2", "rawWater"},
				{"h2", "methane"}, {"h2", "rawWater"}, {"methane", "rawWater"},
			};
			for (auto const& pair : reactorAliases)
			{
				core::StationSystemsSettings settings;
				settings.enabled = true;
				settings.stores = tightStores;
				settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
				settings.sabatierReactors = { reactor("sab", "normal") };
				setReactorRole(settings.sabatierReactors[0], pair.right, pair.left);
				require(!core::validateStationSystemsSettings(settings, diagnostic)
					&& diagnostic.find(alias) != std::string::npos,
					("a Sabatier reactor aliasing its '" + pair.left
						+ "' and '" + pair.right + "' stream roles was accepted").c_str());
			}
			// Methane processor: all three pairs of its three stream roles.
			std::vector<Alias> processorAliases = {
				{"methane", "h2"}, {"methane", "carbon"}, {"h2", "carbon"},
			};
			for (auto const& pair : processorAliases)
			{
				core::StationSystemsSettings settings;
				settings.enabled = true;
				settings.stores = tightStores;
				settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
				settings.methaneProcessors = { processor("pyrolysis", "normal") };
				setProcessorRole(settings.methaneProcessors[0], pair.right, pair.left);
				require(!core::validateStationSystemsSettings(settings, diagnostic)
					&& diagnostic.find(alias) != std::string::npos,
					("a methane processor aliasing its '" + pair.left
						+ "' and '" + pair.right + "' stream roles was accepted").c_str());
			}
			require(!tight.getStationSystemsSnapshot().enabled && tight.getSimulationTick() == 0,
				"the refused aliased-role probes mutated the World");
			// The reported reproduction through the public configure/tick/
			// snapshot seam: the aliased-product reactor was admitted, then
			// advanceTicks(1) failed the capacity audit and left the World
			// paused at tick zero. The alias is now refused at configure,
			// so the overfilling tick never runs.
			{
				core::World world("CarbonAliasRepro", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				core::StationSystemsSettings repro;
				repro.enabled = true;
				repro.stores = {
					plainStore("co2", 100.0, 10.0), plainStore("h2", 100.0, 10.0),
					plainStore("product", 1.0, 0.99), plainStore("water", 100.0, 0.0) };
				repro.thermalReservoirs = { reservoir("sink", "warm", 20.0) };
				core::SolarArraySettings field;
				field.id = "solar";
				field.capacityKW = 100000.0;
				repro.solarArrays = { field };
				core::SabatierReactorSettings aliased;
				aliased.id = "reactor";
				aliased.hazardClass = "normal";
				aliased.co2StoreId = "co2";
				aliased.hydrogenStoreId = "h2";
				aliased.methaneStoreId = "product";
				aliased.waterStoreId = "product";
				aliased.thermalClass = "warm";
				aliased.heatSinkId = "sink";
				aliased.rateKgPerS = 10.0;
				repro.sabatierReactors = { aliased };
				require(!world.configureStationSystems(repro, true, &diagnostic)
					&& diagnostic.find(alias) != std::string::npos,
					"the reproduction's aliased-product Sabatier reactor was accepted");
				require(!world.getStationSystemsSnapshot().enabled
					&& world.getSimulationTick() == 0 && world.getStationSystemsDiagnostics().empty(),
					"the refused aliased-product reproduction mutated the World");
				// The methane-processor variant of the same reproduction.
				repro.sabatierReactors = {};
				core::MethaneProcessorSettings aliasedProcessor;
				aliasedProcessor.id = "processor";
				aliasedProcessor.hazardClass = "normal";
				aliasedProcessor.methaneStoreId = "co2";
				aliasedProcessor.hydrogenStoreId = "product";
				aliasedProcessor.carbonStoreId = "product";
				aliasedProcessor.thermalClass = "warm";
				aliasedProcessor.heatSinkId = "sink";
				aliasedProcessor.rateKgPerS = 10.0;
				aliasedProcessor.specificEnergyKWhPerKgCH4 = 2.0;
				repro.methaneProcessors = { aliasedProcessor };
				require(!world.configureStationSystems(repro, true, &diagnostic)
					&& diagnostic.find(alias) != std::string::npos,
					"the reproduction's aliased-product methane processor was accepted");
				require(!world.getStationSystemsSnapshot().enabled
					&& world.getSimulationTick() == 0 && world.getStationSystemsDiagnostics().empty(),
					"the refused processor-alias reproduction mutated the World");
			}
			// Valid sharing between different machines is preserved: one
			// reactor's methane output store is the processor's methane
			// feed, and both book into the shared hydrogen store. The rule
			// refuses aliases within a machine, never across machines.
			{
				core::World world("CarbonSharedAcrossMachines", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				require(carbonConfigure(world, carbonStores(), { reactor("sab", "normal") },
					{ processor("pyrolysis", "normal") }, 100.0).first,
					"two machines sharing the methane and hydrogen stores were refused");
				world.resumeSimulation();
				require(world.advanceTicks(60), "the shared-store machine run failed");
				require(world.getStationSystemsDiagnostics().empty(),
					"the shared-store machine raised a runtime diagnostic");
				require(world.getStationSystemsSnapshot().sabatierReactors.size() == 1
					&& world.getStationSystemsSnapshot().methaneProcessors.size() == 1,
					"the shared-store machines are missing from the snapshot");
			}
		}
		// Nominal Sabatier conversion to stores: the conversion follows
		// the declared stoichiometry, the energy closes over supplied +
		// released = routed, the exothermic heat lands in the sink, and
		// the per-unit boundary closes.
		{
			core::World world("CarbonSabatier", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(carbonConfigure(world, carbonStores(), { reactor("sab", "normal") }, {}, 100.0).first,
				"nominal Sabatier setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "nominal Sabatier run failed");
			auto const elapsed = 60.0 * dt;
			auto const converted = 0.001 * elapsed;
			auto const h2 = converted * h2PerCo2;
			auto const ch4 = converted * ch4PerCo2;
			auto const water = converted * h2oPerCo2;
			auto const energy = 1.0 * 3600.0 * converted;
			auto const heat = energy + converted * released;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.sabatierReactors.size() == 1, "reactor missing from the snapshot");
			auto const& cell = snap.sabatierReactors[0];
			require(cell.status == core::StationTransferStatus::Full
				&& approx(cell.scale, 1.0) && cell.coolingAvailable,
				"the reactor is not running at full rate with a qualified sink");
			require(approx(cell.co2ConsumedKg, converted) && approx(cell.hydrogenConsumedKg, h2)
				&& approx(cell.methaneProducedKg, ch4) && approx(cell.waterProducedKg, water),
				"the conversion did not follow the declared stoichiometry");
			require(std::abs(cell.residualKg) < 1e-6,
				"the CO2/hydrogen/methane/water ledger does not close");
			require(approx(cell.energySuppliedKJ, energy) && approx(cell.reactionHeatKJ, converted * released)
				&& approx(cell.heatRoutedKJ, heat) && std::abs(cell.residualKJ) < 1e-6,
				"the supplied/released/routed energy ledger does not close");
			require(approx(findStore(snap, "co2").currentQualified, 10.0 - converted)
				&& approx(findStore(snap, "h2").currentQualified, 10.0 - h2),
				"the feed stores lost the wrong amounts");
			require(approx(findStore(snap, "methane").currentQualified, ch4)
				&& approx(findStore(snap, "rawWater").currentQualified, water),
				"the product stores did not receive the conversion");
			require(approx(snap.power.sabatierPowerKW, 1.0 * 3600.0 * 0.001),
				"the power snapshot does not report the reactor draw");
			require(approx(snap.thermal.reservoirs[0].sabatierEnergyKJ, heat)
				&& approx(snap.thermal.reservoirs[0].lastSabatierHeatKW, heat / elapsed),
				"the reaction heat did not land in the sink reservoir");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Nominal methane processing: the split follows the declared
		// stoichiometry, the energy closes over supplied = absorbed +
		// routed, the recovered hydrogen and solid carbon land in their
		// stores, and the per-unit boundary closes.
		{
			core::World world("CarbonPyrolysis", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = carbonStores();
			for (auto& store : stores)
				if (store.id == "methane") store.startingQualified = 10.0;
			require(carbonConfigure(world, stores, {}, { processor("pyrolysis", "normal") }, 100.0).first,
				"nominal methane-processing setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "nominal methane-processing run failed");
			auto const elapsed = 60.0 * dt;
			auto const processed = 0.001 * elapsed;
			auto const h2 = processed * h2PerCh4;
			auto const carbon = processed * cPerCh4;
			auto const energy = 2.0 * 3600.0 * processed;
			auto const absorbed = processed * demand;
			auto const routed = energy - absorbed;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.methaneProcessors.size() == 1, "processor missing from the snapshot");
			auto const& unit = snap.methaneProcessors[0];
			require(unit.status == core::StationTransferStatus::Full
				&& approx(unit.scale, 1.0) && unit.coolingAvailable,
				"the processor is not running at full rate with a qualified sink");
			require(approx(unit.methaneConsumedKg, processed)
				&& approx(unit.hydrogenReturnedKg, h2) && approx(unit.carbonProducedKg, carbon),
				"the split did not follow the declared stoichiometry");
			require(std::abs(unit.residualKg) < 1e-6,
				"the methane/hydrogen/carbon split ledger does not close");
			require(approx(unit.energySuppliedKJ, energy) && approx(unit.reactionHeatKJ, absorbed)
				&& approx(unit.heatRoutedKJ, routed) && std::abs(unit.residualKJ) < 1e-6,
				"the supplied/absorbed/routed energy ledger does not close");
			require(approx(findStore(snap, "methane").currentQualified, 10.0 - processed)
				&& approx(findStore(snap, "h2").currentQualified, 10.0 + h2)
				&& approx(findStore(snap, "carbon").currentQualified, carbon),
				"the feed and product stores moved the wrong amounts");
			require(approx(snap.power.methaneProcessingPowerKW, 2.0 * 3600.0 * 0.001),
				"the power snapshot does not report the processor draw");
			require(approx(snap.thermal.reservoirs[0].methaneProcessingEnergyKJ, routed),
				"the process waste heat did not land in the sink reservoir");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Integrated chain: the reactor feeds the processor through the
		// methane store and the processor returns hydrogen through the
		// hydrogen store. The combined process retains a net hydrogen
		// demand of exactly half the reactor's draw, so the recycled
		// hydrogen can never keep the chain running: with a finite
		// hydrogen store the chain drains it at the net rate and grinds
		// to a halt while CO2 still has plenty of room.
		{
			core::World world("CarbonChain", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const startingHydrogen = 0.0022;
			auto chain = [&]()
			{
				std::vector<core::ResourceStoreSettings> stores;
				stores.push_back(plainStore("co2", 100.0, 10.0));
				stores.push_back(plainStore("h2", 100.0, startingHydrogen));
				stores.push_back(plainStore("methane", 100.0, 0.0));
				stores.push_back(plainStore("rawWater", 100.0, 0.0));
				stores.push_back(plainStore("carbon", 100.0, 0.0));
				return stores;
			};
			// The processor is rated at exactly the methane the reactor
			// produces, so the chain runs at the reactor's pace.
			auto chainProcessor = processor("pyrolysis", "normal");
			chainProcessor.rateKgPerS = 0.001 * ch4PerCo2;
			require(carbonConfigure(world, chain(), { reactor("sab", "normal") },
				{ chainProcessor }, 100.0).first,
				"integrated carbon-chain setup refused");
			world.resumeSimulation();
			// The net drain is h2PerCo2 / 2 per CO2, so 0.0022 kg of
			// hydrogen sustains about 1441 ticks of the chain at the
			// 1/60 s fixed timestep; run well past the exhaustion into
			// the asymptotic decay tail.
			require(world.advanceTicks(1550), "integrated carbon-chain run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.sabatierReactors[0];
			auto const& unit = snap.methaneProcessors[0];
			require(cell.co2ConsumedKg > 0.0 && unit.methaneConsumedKg > 0.0,
				"the chain did not convert and process");
			require(approx(unit.methaneConsumedKg, cell.methaneProducedKg),
				"the processor did not consume the methane the reactor produced");
			// Net hydrogen: reactor draw minus recycled return must equal
			// half the reactor draw - the recycled share never covers the
			// demand, so the store keeps draining.
			auto const netHydrogen = cell.hydrogenConsumedKg - unit.hydrogenReturnedKg;
			require(approx(netHydrogen, cell.hydrogenConsumedKg / 2.0),
				"the combined chain does not retain the declared net hydrogen demand");
			require(approx(findStore(snap, "h2").currentQualified, startingHydrogen - netHydrogen),
				"the hydrogen store does not reflect the net demand");
			require(unit.carbonProducedKg > 0.0
				&& approx(findStore(snap, "carbon").currentQualified, unit.carbonProducedKg),
				"the solid carbon did not land in its store");
			// The chain bought exactly its net hydrogen supply times two
			// over the demand ratio of CO2 conversion, and is now
			// hydrogen-starved: the store is spent and the reactor names
			// the hydrogen store as its limiter.
			require(closeTo(cell.co2ConsumedKg, 2.0 * startingHydrogen / h2PerCo2, 0.01),
				"the total CO2 converted is not the net hydrogen supply times two over the demand ratio");
			require(findStore(snap, "h2").currentQualified < 1e-9,
				"the hydrogen store was not drained by the net demand");
			require(cell.limiter.find("hydrogen feed store") != std::string::npos
				|| cell.blockReason.find("hydrogen feed store") != std::string::npos,
				"the starved chain did not name the hydrogen store as its limiter");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Blocked destinations refuse or cap without gifting product: a
		// full methane store stops the reactor, a full carbon store stops
		// the processor, and a filling water store caps the reactor once
		// its headroom is gone - each naming the blocking store instead
		// of venting.
		{
			core::World world("MethaneFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = carbonStores();
			for (auto& store : stores)
				if (store.id == "methane") { store.capacity = 0.0005; store.startingQualified = 0.0005; }
			require(carbonConfigure(world, stores, { reactor("sab", "normal") }, {}, 100.0).first,
				"full-methane setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "full-methane run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.sabatierReactors[0].status == core::StationTransferStatus::Rejected
				&& snap.sabatierReactors[0].co2ConsumedKg == 0.0
				&& snap.sabatierReactors[0].blockReason.find("methane store") != std::string::npos,
				"a full methane store did not refuse the reactor with a named reason");
			require(findStore(snap, "methane").currentQualified <= 0.0005 + 1e-9,
				"a blocked destination was overfilled");
		}
		{
			core::World world("CarbonFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = carbonStores();
			for (auto& store : stores)
			{
				if (store.id == "carbon") { store.capacity = 0.0005; store.startingQualified = 0.0005; }
				if (store.id == "methane") store.startingQualified = 10.0;
			}
			require(carbonConfigure(world, stores, {}, { processor("pyrolysis", "normal") }, 100.0).first,
				"full-carbon setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "full-carbon run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.methaneProcessors[0].status == core::StationTransferStatus::Rejected
				&& snap.methaneProcessors[0].methaneConsumedKg == 0.0
				&& snap.methaneProcessors[0].blockReason.find("solid carbon") != std::string::npos,
				"a full carbon store did not refuse the processor with a named reason");
			require(approx(findStore(snap, "methane").currentQualified, 10.0),
				"the methane feed moved while the carbon destination was full");
		}
		{
			core::World world("WaterFills", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = carbonStores();
			for (auto& store : stores)
				if (store.id == "rawWater") store.capacity = 0.0002;
			require(carbonConfigure(world, stores, { reactor("sab", "normal") }, {}, 100.0).first,
				"filling-water setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "filling-water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(approx(findStore(snap, "rawWater").currentQualified, 0.0002),
				"the water store did not fill to capacity");
			require(snap.sabatierReactors[0].limiter.find("water store") != std::string::npos
				|| snap.sabatierReactors[0].blockReason.find("water store") != std::string::npos,
				"the filled water store did not become the named limiter");
			// The whole run converted at most the water capacity's worth.
			require(snap.sabatierReactors[0].co2ConsumedKg <= 0.0002 / h2oPerCo2 + 1e-9,
				"the reactor converted past the water store's capacity");
		}
		// Blocked destinations cap throughput: a full methane store caps
		// the reactor and a full carbon store caps the processor, each
		// naming the blocking store instead of venting.
		{
			core::World world("CarbonBlocked", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = carbonStores();
			for (auto& store : stores)
			{
				if (store.id == "methane") { store.capacity = 0.0005; store.startingQualified = 0.0005; }
				if (store.id == "carbon") { store.capacity = 0.0005; store.startingQualified = 0.0005; }
			}
			require(carbonConfigure(world, stores, { reactor("sab", "normal") },
				{ processor("pyrolysis", "normal") }, 100.0).first,
				"blocked-destination setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "blocked-destination run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.sabatierReactors[0].scale < 1.0
				&& snap.sabatierReactors[0].limiter.find("methane store") != std::string::npos,
				"a full methane store did not cap the reactor with a named limiter");
			require(snap.methaneProcessors[0].scale < 1.0
				&& snap.methaneProcessors[0].limiter.find("carbon store") != std::string::npos,
				"a full carbon store did not cap the processor with a named limiter");
			require(findStore(snap, "methane").currentQualified <= 0.0005 + 1e-9
				&& findStore(snap, "carbon").currentQualified <= 0.0005 + 1e-9,
				"a blocked destination was overfilled");
		}
		// Disabled stages: a zero rated rate is idle, not broken - no
		// flows, no power draw, and the disabled flag is set.
		{
			core::World world("CarbonDisabled", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto offReactor = reactor("sab", "normal");
			offReactor.rateKgPerS = 0.0;
			auto offProcessor = processor("pyrolysis", "normal");
			offProcessor.rateKgPerS = 0.0;
			require(carbonConfigure(world, carbonStores(), { offReactor }, { offProcessor }, 100.0).first,
				"disabled-stage setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "disabled-stage run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.sabatierReactors[0].disabled
				&& snap.sabatierReactors[0].status == core::StationTransferStatus::Idle
				&& snap.sabatierReactors[0].co2ConsumedKg == 0.0
				&& snap.sabatierReactors[0].lastPowerSuppliedKW == 0.0,
				"a zero-rate reactor is not an idle, zero-flow stage");
			require(snap.methaneProcessors[0].disabled
				&& snap.methaneProcessors[0].status == core::StationTransferStatus::Idle
				&& snap.methaneProcessors[0].methaneConsumedKg == 0.0
				&& snap.methaneProcessors[0].lastPowerSuppliedKW == 0.0,
				"a zero-rate processor is not an idle, zero-flow stage");
		}
		// Cooling gate: a sink at or above the maximum refuses both
		// stages whole - no flows, no power draw, reason named.
		{
			core::World world("CarbonHotSink", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(carbonConfigure(world, carbonStores(), { reactor("sab", "normal") },
				{ processor("pyrolysis", "normal") }, 100.0, 70.0).first,
				"hot-sink carbon setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "hot-sink carbon run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.sabatierReactors[0].status == core::StationTransferStatus::Rejected
				&& !snap.sabatierReactors[0].coolingAvailable
				&& snap.sabatierReactors[0].co2ConsumedKg == 0.0
				&& snap.sabatierReactors[0].lastPowerSuppliedKW == 0.0
				&& snap.sabatierReactors[0].blockReason.find("cooling is not qualified") != std::string::npos,
				"a hot sink did not refuse the reactor whole");
			require(snap.methaneProcessors[0].status == core::StationTransferStatus::Rejected
				&& !snap.methaneProcessors[0].coolingAvailable
				&& snap.methaneProcessors[0].methaneConsumedKg == 0.0
				&& snap.methaneProcessors[0].lastPowerSuppliedKW == 0.0,
				"a hot sink did not refuse the processor whole");
		}
		// Power starvation: with no generation and no battery, a tick
		// that demands power receives none and is refused whole - no
		// reactants move and the reason is named.
		{
			core::World world("CarbonNoPower", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto noPowerStores = carbonStores();
			for (auto& store : noPowerStores)
				if (store.id == "methane") store.startingQualified = 10.0;
			require(carbonConfigure(world, noPowerStores, { reactor("sab", "normal") },
				{ processor("pyrolysis", "normal") }, 0.0).first,
				"no-power carbon setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "no-power carbon run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.sabatierReactors[0].status == core::StationTransferStatus::Rejected
				&& snap.sabatierReactors[0].co2ConsumedKg == 0.0
				&& snap.sabatierReactors[0].blockReason.find("no electrical supply") != std::string::npos,
				"the reactor ran or drew reactants without power");
			require(snap.methaneProcessors[0].status == core::StationTransferStatus::Rejected
				&& snap.methaneProcessors[0].methaneConsumedKg == 0.0
				&& snap.methaneProcessors[0].blockReason.find("no electrical supply") != std::string::npos,
				"the processor ran or drew reactants without power");
			require(approx(findStore(snap, "co2").currentQualified, 10.0)
				&& approx(findStore(snap, "h2").currentQualified, 10.0),
				"reactants moved without power");
		}
		// Protected reserves: the feeds are drawn only above each store's
		// protected reserve, so a protected hydrogen reserve is never
		// converted away - the chain stops at the reserve line, not
		// through it.
		{
			core::World world("CarbonReserve", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = carbonStores();
			for (auto& store : stores)
				if (store.id == "h2") store.protectedReserve = 9.98;
			auto bigReactor = reactor("sab", "normal");
			bigReactor.rateKgPerS = 0.01;
			require(carbonConfigure(world, stores, { bigReactor }, {}, 100.0).first,
				"protected-reserve carbon setup refused");
			world.resumeSimulation();
			// 0.02 kg of drawable hydrogen at 0.01 kg/s CO2 (0.001832 kg/s
			// hydrogen) lasts about 655 ticks at the 1/60 s fixed
			// timestep; run past the exhaustion.
			require(world.advanceTicks(700), "protected-reserve carbon run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(findStore(snap, "h2").currentQualified >= 9.98 - 1e-9,
				"the reactor drew into the protected hydrogen reserve");
			require(closeTo(snap.sabatierReactors[0].co2ConsumedKg, 0.02 / h2PerCo2, 0.01),
				"the conversion did not stop at the drawable hydrogen line");
			require(snap.sabatierReactors[0].limiter.find("hydrogen feed store") != std::string::npos
				|| snap.sabatierReactors[0].blockReason.find("hydrogen feed store") != std::string::npos,
				"the reserve-capped reactor did not name the hydrogen store");
		}
	}

	void nutrientRecovery(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::max(1e-12, std::abs(expected));
		};
		// The authored feedstock: a wet organic waste whose dry mass carries
		// bound nutrient nitrogen, phosphorus, potassium and grouped
		// minerals, plus an atmospheric nitrogen share that is NOT a
		// nutrient. The declared yields recover 80% of N/P/K, 70% of the
		// grouped minerals and 70% of the moisture; the residue is the
		// mass-balance remainder.
		core::ResourceCompositionSettings feedComposition;
		feedComposition.moisture = 0.9;
		feedComposition.nutrientNitrogen = 0.05;
		feedComposition.atmosphericNitrogen = 0.02;
		feedComposition.phosphorus = 0.02;
		feedComposition.potassium = 0.02;
		feedComposition.otherMinerals = 0.03;
		feedComposition.energyKJPerKgDry = 8000.0;
		auto const dryPerKg = 1.0 - feedComposition.moisture;
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
		auto compoundStore = [&](std::string const& id, double capacity, double starting,
			core::ResourceCompositionSettings const& composition)
		{
			auto settings = plainStore(id, capacity, starting);
			settings.composition = composition;
			return settings;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = 10000.0;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto processor = [](std::string const& id, std::string const& hazard)
		{
			core::MicrobialProcessorSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.feedStoreId = "waste";
			settings.oxygenStoreId = "oxygen";
			settings.waterStoreId = "processWater";
			settings.consumableStoreId = "media";
			settings.nutrientStoreId = "nutrients";
			settings.recoveredWaterStoreId = "recoveredWater";
			settings.co2StoreId = "co2";
			settings.residueStoreId = "residue";
			settings.thermalClass = "low";
			settings.heatSinkId = "sink";
			settings.rateKgPerS = 0.001;
			settings.specificEnergyKWhPerKgFeed = 2.0;
			settings.oxygenPerKgFeedKg = 0.8;
			settings.processWaterPerKgFeedKg = 0.5;
			settings.consumablePerKgFeed = 0.01;
			settings.co2PerKgFeedKg = 0.3;
			settings.nitrogenRecovery = 0.8;
			settings.phosphorusRecovery = 0.8;
			settings.potassiumRecovery = 0.8;
			settings.mineralsRecovery = 0.7;
			settings.waterRecovery = 0.7;
			settings.reactionHeatKJPerKgFeed = 600.0;
			settings.minOperatingTemperatureC = 15.0;
			settings.maxSinkTemperatureC = 45.0;
			return settings;
		};
		auto nutrientStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(compoundStore("waste", 100.0, 10.0, feedComposition));
			stores.push_back(plainStore("oxygen", 100.0, 10.0));
			stores.push_back(plainStore("processWater", 100.0, 10.0));
			stores.push_back(plainStore("media", 100.0, 10.0));
			stores.push_back(plainStore("nutrients", 100.0, 0.0));
			stores.push_back(plainStore("recoveredWater", 100.0, 0.0));
			stores.push_back(plainStore("co2", 100.0, 0.0));
			stores.push_back(plainStore("residue", 100.0, 0.0));
			return stores;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto nutrientConfigure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::MicrobialProcessorSettings> processors,
			double solarKW,
			double sinkStartingC = 20.0)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.thermalReservoirs = { reservoir("sink", "low", sinkStartingC) };
			settings.microbialProcessors = std::move(processors);
			if (solarKW > 0.0)
			{
				core::SolarArraySettings field;
				field.id = "field";
				field.capacityKW = solarKW;
				settings.solarArrays = { field };
			}
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// The declared per-kilogram-of-feed yields the nominal scenario
		// must follow, derived from the feedstock composition.
		auto const nutrientPerKgFeed = dryPerKg * (feedComposition.nutrientNitrogen * 0.8
			+ feedComposition.phosphorus * 0.8 + feedComposition.potassium * 0.8
			+ feedComposition.otherMinerals * 0.7);
		auto const recoveredWaterPerKgFeed = feedComposition.moisture * 0.7;
		auto const residuePerKgFeed = 1.0 + 0.8 + 0.5 - nutrientPerKgFeed
			- recoveredWaterPerKgFeed - 0.3;
		require(nutrientPerKgFeed > 0.0 && residuePerKgFeed > 0.0,
			"the nominal feedstock/yield declaration is not a sane recovery scenario");
		// Refused configurations: every malformed nutrient-recovery contract
		// is rejected before anything runs, and none of the refusals
		// mutates the World.
		{
			core::World bad("NutrientBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = nutrientStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
			std::string diagnostic;
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].feedStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("feed store 'ghost' does not exist") != std::string::npos,
				"a microbial processor fed by an unknown store was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].feedStoreId = "oxygen";
			settings.microbialProcessors[0].oxygenStoreId = "oxygen2";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("no composition") != std::string::npos,
				"a processor fed by a store with no declared composition was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].nutrientStoreId = "waste";
			settings.microbialProcessors[0].feedStoreId = "nutrients";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("feed store 'nutrients' declares no composition") != std::string::npos,
				"a swap of the compound feed onto a plain store was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.stores.push_back(compoundStore("sludge", 10.0, 1.0, feedComposition));
			settings.microbialProcessors[0].residueStoreId = "sludge";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("residue store 'sludge' declares a composition") != std::string::npos,
				"a compound residue destination was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].nitrogenRecovery = 1.5;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("nitrogen recovery must be finite and from 0 to 1") != std::string::npos,
				"a recovery yield above 1 was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].co2PerKgFeedKg = 2.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("would create mass") != std::string::npos,
				"yields whose outputs exceed their inputs were accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].reactionHeatKJPerKgFeed = 900.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("exceeds the feedstock's declared chemical energy") != std::string::npos,
				"a reaction heat above the feed's declared chemical energy was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].minOperatingTemperatureC = 50.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("empty operating window") != std::string::npos,
				"an empty operating temperature window was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].oxygenStoreId = "processWater";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("same store") != std::string::npos,
				"two chain roles sharing one store were accepted");
			settings.microbialProcessors = { processor("digester", "normal"),
				processor("digester2", "hazardous") };
			// A hazardous processor sharing the normal-class waste feed is
			// accepted (#751): the shared inventory is the consequence.
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"a hazardous processor sharing the normal-class waste feed was refused");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].heatSinkId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown thermal reservoir") != std::string::npos,
				"a processor on an unknown heat sink was accepted");
			// #408 condition/recovery contract refusals.
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].initialCondition = 1.5;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("initial culture condition must be finite and from 0 to 1")
					!= std::string::npos,
				"an initial culture condition above 1 was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].starvationDegradePerHour = -0.1;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("starvation degradation per hour must be finite and non-negative")
					!= std::string::npos,
				"a negative starvation degradation rate was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].recoveryPerHour = -1.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("recovery per hour must be finite and non-negative")
					!= std::string::npos,
				"a negative recovery rate was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].stockStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("replacement biological stock store 'ghost' does not exist")
					!= std::string::npos,
				"a processor on an unknown replacement stock store was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].stockStoreId = "waste";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("feed store and replacement stock store are the same store")
					!= std::string::npos,
				"a replacement stock store doubling as the feed store was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].stockStoreId = "media";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("consumable store and replacement stock store are the same store")
					!= std::string::npos,
				"a replacement stock store doubling as the consumable was accepted");
			settings.stores.push_back(plainStore("stock", 10.0, 1.0));
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].stockStoreId = "stock";
			settings.microbialProcessors[0].stockUnitsPerConditionPoint = 0.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("stock units per condition point must be positive when a replacement biological stock store is declared")
					!= std::string::npos,
				"a declared stock store with zero units per condition point was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].stockStoreId = "stock";
			settings.microbialProcessors[0].stockUnitsPerConditionPoint = -2.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("stock units per condition point must be finite and non-negative")
					!= std::string::npos,
				"a negative stock units per condition point was accepted");
			// A hazardous processor may draw the normal-class stock store
			// the normal processor uses (#751): the authored-class check is
			// gone, so the cross-class draw configures and the shared
			// inventory is the consequence.
			{
				auto hStores = settings.stores;
				auto hazardousStore = [](core::ResourceStoreSettings store)
				{ store.hazardClass = "hazardous"; return store; };
				hStores.push_back(hazardousStore(compoundStore("h_waste", 100.0, 10.0, feedComposition)));
				hStores.push_back(hazardousStore(plainStore("h_oxygen", 100.0, 10.0)));
				hStores.push_back(hazardousStore(plainStore("h_water", 100.0, 10.0)));
				hStores.push_back(hazardousStore(plainStore("h_media", 100.0, 10.0)));
				hStores.push_back(hazardousStore(plainStore("h_nutrients", 100.0, 0.0)));
				hStores.push_back(hazardousStore(plainStore("h_recoveredWater", 100.0, 0.0)));
				hStores.push_back(hazardousStore(plainStore("h_co2", 100.0, 0.0)));
				hStores.push_back(hazardousStore(plainStore("h_residue", 100.0, 0.0)));
				core::StationSystemsSettings hazardSettings;
				hazardSettings.enabled = true;
				hazardSettings.stores = hStores;
				hazardSettings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
				auto normal = processor("digester", "normal");
				normal.stockStoreId = "stock";
				auto hazardous = processor("digester2", "hazardous");
				hazardous.feedStoreId = "h_waste";
				hazardous.oxygenStoreId = "h_oxygen";
				hazardous.waterStoreId = "h_water";
				hazardous.consumableStoreId = "h_media";
				hazardous.nutrientStoreId = "h_nutrients";
				hazardous.recoveredWaterStoreId = "h_recoveredWater";
				hazardous.co2StoreId = "h_co2";
				hazardous.residueStoreId = "h_residue";
				hazardous.stockStoreId = "stock";
				hazardSettings.microbialProcessors = { normal, hazardous };
				require(core::validateStationSystemsSettings(hazardSettings, diagnostic),
					("a hazardous processor drawing the normal-class replacement stock store was refused: " + diagnostic).c_str());
			}
			// #751: the consumable and replacement-stock roles are no
			// longer class-bound. Every cross-class combination that #592
			// once refuses now validates: the processor draws whatever
			// media it is wired to, and the quantity it draws is the
			// whole of the report.
			{
				core::StationSystemsSettings probe;
				probe.enabled = true;
				probe.stores = nutrientStores();
				probe.stores.push_back(plainStore("stock", 100.0, 10.0));
				probe.thermalReservoirs = { reservoir("sink", "low", 20.0) };
				probe.microbialProcessors = { processor("digester", "normal") };
				probe.microbialProcessors[0].stockStoreId = "stock";
				probe.microbialProcessors[0].stockUnitsPerConditionPoint = 1.0;
				// A normal processor drawing hazardous-class media is accepted.
				probe.stores[3].hazardClass = "hazardous";
				require(core::validateStationSystemsSettings(probe, diagnostic),
					("a normal processor drawing hazardous-class media was refused: " + diagnostic).c_str());
				// ...and hazardous-class replacement stock instead.
				probe.stores.back().hazardClass = "hazardous";
				require(core::validateStationSystemsSettings(probe, diagnostic),
					("a normal processor drawing hazardous-class replacement stock was refused: " + diagnostic).c_str());
				// The reverse direction too: a hazardous processor drawing
				// normal-class media and stock.
				for (auto& entry : probe.stores)
					entry.hazardClass = "hazardous";
				probe.microbialProcessors = { processor("digester", "hazardous") };
				probe.microbialProcessors[0].stockStoreId = "stock";
				probe.microbialProcessors[0].stockUnitsPerConditionPoint = 1.0;
				probe.stores[3].hazardClass = "normal";
				probe.stores.back().hazardClass = "normal";
				require(core::validateStationSystemsSettings(probe, diagnostic),
					("a hazardous processor drawing normal-class media and stock was refused: " + diagnostic).c_str());
				// The fully class-matched hazardous chain still validates.
				probe.stores[3].hazardClass = "hazardous";
				probe.stores.back().hazardClass = "hazardous";
				require(core::validateStationSystemsSettings(probe, diagnostic),
					("the fully hazardous-class chain was refused: " + diagnostic).c_str());
			}
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].thermalClass = "warm";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("does not match heat sink") != std::string::npos,
				"a thermal class mismatch against the processor's heat sink was accepted");
			std::vector<core::MicrobialProcessorSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationMicrobialProcessors; ++index)
				tooMany.push_back(processor("digester" + std::to_string(index), "normal"));
			settings = core::StationSystemsSettings{};
			settings.enabled = true;
			settings.stores = nutrientStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
			settings.microbialProcessors = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared microbial processor budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused nutrient-recovery configuration mutated the World");
		}
		// The mirrored consequence through the public configure/tick/
		// snapshot seam: the reviewed probe configuration - a normal
		// processor with hazardous-class media and replacement stock while
		// every other stream is normal - is now accepted (#751), runs,
		// and draws the cross-class media and stock it is wired to.
		{
			core::World world("MicrobialCrossClassDraw", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = nutrientStores();
			stores.push_back(plainStore("stock", 100.0, 10.0));
			stores[3].hazardClass = "hazardous";
			stores.back().hazardClass = "hazardous";
			auto probe = processor("digester", "normal");
			probe.stockStoreId = "stock";
			probe.stockUnitsPerConditionPoint = 1.0;
			// Start below full condition so recovery draws replacement
			// stock within the run (#408 coefficients).
			probe.initialCondition = 0.5;
			probe.recoveryPerHour = 1.0;
			require(nutrientConfigure(world, stores, { probe }, 100.0).first,
				"the cross-class media/stock configuration was refused through the public seam");
			world.resumeSimulation();
			require(world.advanceTicks(60), "the cross-class media/stock rig failed to tick");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.enabled && snap.microbialProcessors.size() == 1,
				"the cross-class media/stock rig did not run");
			auto const media = findStore(snap, "media");
			auto const stock = findStore(snap, "stock");
			require(media.currentQualified < 10.0 && media.currentQualified > 0.0,
				"the normal processor did not draw the hazardous-class media it is wired to");
			require(stock.currentQualified < 10.0 && stock.currentQualified > 0.0,
				"the normal processor did not draw the hazardous-class replacement stock while recovering");
			require(snap.microbialProcessors[0].condition > 0.5,
				"the cross-class rig did not recover condition");
		}
		// The mirrored positive case: the same rig with every store authored
		// hazardous-class and a hazardous processor ticks and consumes its
		// hazardous media and replacement stock normally - the #592 check
		// refuses mismatches, not legitimate hazardous operation.
		{
			core::World world("MicrobialHazardNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = nutrientStores();
			stores.push_back(plainStore("stock", 100.0, 10.0));
			for (auto& entry : stores)
				entry.hazardClass = "hazardous";
			auto probe = processor("digester", "hazardous");
			probe.stockStoreId = "stock";
			probe.stockUnitsPerConditionPoint = 1.0;
			// Start below full condition so recovery draws replacement
			// stock within the run (#408 coefficients).
			probe.initialCondition = 0.5;
			probe.recoveryPerHour = 1.0;
			require(nutrientConfigure(world, stores, { probe }, 100.0).first,
				"the fully hazardous-class rig was refused through the public seam");
			world.resumeSimulation();
			require(world.advanceTicks(60), "the fully hazardous-class rig failed to tick");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.enabled && snap.microbialProcessors.size() == 1,
				"the fully hazardous-class rig did not run");
			auto const media = findStore(snap, "media");
			auto const stock = findStore(snap, "stock");
			require(media.currentQualified < 10.0 && media.currentQualified > 0.0,
				"the hazardous processor did not draw its hazardous-class media");
			require(stock.currentQualified < 10.0 && stock.currentQualified > 0.0,
				"the hazardous processor did not draw its hazardous-class replacement stock while recovering");
			require(snap.microbialProcessors[0].condition > 0.5,
				"the hazardous processor did not recover condition from its hazardous stock");
		}
		// Nominal digestion: the recovery follows the declared yields, the
		// per-constituent recovery is funded by the feed's bound nutrients,
		// the residue closes the mass balance, the energy closes over
		// supplied + released = routed, and the per-unit and per-
		// constituent boundary ledgers close.
		{
			core::World world("NutrientNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(nutrientConfigure(world, nutrientStores(), { processor("digester", "normal") },
				100.0).first, "nominal nutrient-recovery setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "nominal nutrient-recovery run failed");
			auto const elapsed = 60.0 * dt;
			auto const fed = 0.001 * elapsed;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.microbialProcessors.size() == 1, "processor missing from the snapshot");
			auto const& unit = snap.microbialProcessors[0];
			require(unit.status == core::StationTransferStatus::Full
				&& approx(unit.scale, 1.0) && unit.temperatureEligible,
				"the processor is not running at full rate inside its operating window");
			require(approx(unit.feedConsumedKg, fed)
				&& approx(unit.oxygenConsumedKg, fed * 0.8)
				&& approx(unit.waterConsumedKg, fed * 0.5)
				&& approx(unit.consumablesUsed, fed * 0.01),
				"the digestion inputs did not follow the declared rates");
			require(approx(unit.nutrientProducedKg, fed * nutrientPerKgFeed)
				&& approx(unit.recoveredWaterKg, fed * recoveredWaterPerKgFeed)
				&& approx(unit.co2ProducedKg, fed * 0.3)
				&& approx(unit.residueProducedKg, fed * residuePerKgFeed),
				"the recovery did not follow the declared yields");
			// Elemental balance: each recovered nutrient is the feed's
			// bound content times its recovery fraction.
			require(approx(unit.nitrogenRecoveredKg, fed * dryPerKg * feedComposition.nutrientNitrogen * 0.8)
				&& approx(unit.phosphorusRecoveredKg, fed * dryPerKg * feedComposition.phosphorus * 0.8)
				&& approx(unit.potassiumRecoveredKg, fed * dryPerKg * feedComposition.potassium * 0.8)
				&& approx(unit.mineralsRecoveredKg, fed * dryPerKg * feedComposition.otherMinerals * 0.7),
				"the per-constituent recovery does not match the feed's bound content times the declared yields");
			require(std::abs(unit.residualKg) < 1e-6,
				"the feed/oxygen/water/nutrient/water/CO2/residue mass ledger does not close");
			// Energy: the dose plus the capped exothermic heat routed once.
			auto const energy = 2.0 * 3600.0 * fed;
			auto const released = 600.0 * fed;
			require(approx(unit.energySuppliedKJ, energy) && approx(unit.reactionHeatKJ, released)
				&& approx(unit.heatRoutedKJ, energy + released) && std::abs(unit.residualKJ) < 1e-6,
				"the supplied/released/routed energy ledger does not close");
			require(released <= fed * dryPerKg * feedComposition.energyKJPerKgDry + 1e-9,
				"the released reaction heat exceeded the feedstock's chemical energy");
			require(approx(findStore(snap, "waste").currentQualified, 10.0 - fed)
				&& approx(findStore(snap, "oxygen").currentQualified, 10.0 - fed * 0.8)
				&& approx(findStore(snap, "processWater").currentQualified, 10.0 - fed * 0.5)
				&& approx(findStore(snap, "media").currentQualified, 10.0 - fed * 0.01),
				"the input stores lost the wrong amounts");
			require(approx(findStore(snap, "nutrients").currentQualified, fed * nutrientPerKgFeed)
				&& approx(findStore(snap, "recoveredWater").currentQualified, fed * recoveredWaterPerKgFeed)
				&& approx(findStore(snap, "co2").currentQualified, fed * 0.3)
				&& approx(findStore(snap, "residue").currentQualified, fed * residuePerKgFeed),
				"the product stores did not receive the recovery");
			require(approx(snap.power.microbialProcessingPowerKW, 2.0 * 3600.0 * 0.001),
				"the power snapshot does not report the processor draw");
			require(approx(snap.thermal.reservoirs[0].microbialEnergyKJ, energy + released)
				&& approx(snap.thermal.reservoirs[0].lastMicrobialHeatKW, (energy + released) / elapsed),
				"the digestion heat did not land in the sink reservoir");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
			for (auto const& balance : snap.constituentBalances)
				require(std::abs(balance.residual) <= 2e-6,
					("constituent boundary residual for '" + balance.constituent + "' not closed").c_str());
		}
		// Atmospheric nitrogen cannot substitute: a feedstock whose
		// nitrogen is entirely atmospheric recovers no nutrient nitrogen at
		// all - the atmospheric share passes through to the residue, and
		// only the other bound constituents are recovered.
		{
			core::World world("NutrientAtmospheric", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto atmosphericFeed = feedComposition;
			atmosphericFeed.nutrientNitrogen = 0.0;
			atmosphericFeed.atmosphericNitrogen = 0.07;
			auto stores = nutrientStores();
			stores[0] = compoundStore("waste", 100.0, 10.0, atmosphericFeed);
			require(nutrientConfigure(world, stores, { processor("digester", "normal") },
				100.0).first, "atmospheric-feed setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "atmospheric-feed run failed");
			auto const elapsed = 60.0 * dt;
			auto const fed = 0.001 * elapsed;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& unit = snap.microbialProcessors[0];
			require(unit.status == core::StationTransferStatus::Full,
				"the atmospheric-feed processor is not running at full rate");
			require(unit.nitrogenRecoveredKg == 0.0,
				"atmospheric nitrogen was recovered as a nutrient without a configured process");
			auto const otherNutrientPerKg = dryPerKg * (atmosphericFeed.phosphorus * 0.8
				+ atmosphericFeed.potassium * 0.8 + atmosphericFeed.otherMinerals * 0.7);
			require(approx(unit.nutrientProducedKg, fed * otherNutrientPerKg),
				"the non-nitrogen nutrients did not follow the declared yields");
			require(approx(unit.phosphorusRecoveredKg, fed * dryPerKg * atmosphericFeed.phosphorus * 0.8),
				"the phosphorus recovery does not match the feed's bound content");
			require(std::abs(unit.residualKg) < 1e-6,
				"the atmospheric-feed mass ledger does not close");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
			for (auto const& balance : snap.constituentBalances)
				require(std::abs(balance.residual) <= 2e-6,
					("constituent boundary residual for '" + balance.constituent + "' not closed").c_str());
		}
		// Absent feedstock: with no qualified waste the processor is
		// refused whole - no oxygen, water or consumable is drawn, no
		// power is spent, and the limiter names the feed store.
		{
			core::World world("NutrientNoFeed", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = nutrientStores();
			stores[0].startingQualified = 0.0;
			require(nutrientConfigure(world, stores, { processor("digester", "normal") },
				100.0).first, "absent-feedstock setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "absent-feedstock run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.microbialProcessors[0].status == core::StationTransferStatus::Rejected
				&& snap.microbialProcessors[0].feedConsumedKg == 0.0
				&& snap.microbialProcessors[0].lastPowerSuppliedKW == 0.0
				&& snap.microbialProcessors[0].blockReason.find("feed store") != std::string::npos,
				"the processor ran or spent resources without feedstock");
			require(approx(findStore(snap, "oxygen").currentQualified, 10.0)
				&& approx(findStore(snap, "nutrients").currentQualified, 0.0),
				"resources moved while the feed store was empty");
		}
		// Power starvation: with no generation and no battery, a tick
		// that demands power receives none and is refused whole - no feed
		// digests and the reason is named.
		{
			core::World world("NutrientNoPower", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(nutrientConfigure(world, nutrientStores(), { processor("digester", "normal") },
				0.0).first, "no-power setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "no-power run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.microbialProcessors[0].status == core::StationTransferStatus::Rejected
				&& snap.microbialProcessors[0].feedConsumedKg == 0.0
				&& snap.microbialProcessors[0].blockReason.find("no electrical supply") != std::string::npos,
				"the processor ran or drew feed without power");
			require(approx(findStore(snap, "waste").currentQualified, 10.0)
				&& approx(findStore(snap, "oxygen").currentQualified, 10.0),
				"inputs moved without power");
		}
		// Temperature eligibility: the static operating window refuses the
		// stage whole on either side - too cold the culture is dormant,
		// too hot the heat cannot be rejected - with no flows and no power
		// draw either way.
		{
			core::World cold("NutrientColdSink", 2, 2);
			cold.pauseSimulation(); cold.finishBuild();
			require(nutrientConfigure(cold, nutrientStores(), { processor("digester", "normal") },
				100.0, 10.0).first, "cold-sink setup refused");
			cold.resumeSimulation();
			require(cold.advanceTicks(60), "cold-sink run failed");
			auto const snap = cold.getStationSystemsSnapshot();
			require(snap.microbialProcessors[0].status == core::StationTransferStatus::Rejected
				&& !snap.microbialProcessors[0].temperatureEligible
				&& snap.microbialProcessors[0].feedConsumedKg == 0.0
				&& snap.microbialProcessors[0].lastPowerSuppliedKW == 0.0
				&& snap.microbialProcessors[0].blockReason.find("minimum operating temperature") != std::string::npos,
				"a below-window sink temperature did not refuse the processor whole");
			core::World hot("NutrientHotSink", 2, 2);
			hot.pauseSimulation(); hot.finishBuild();
			require(nutrientConfigure(hot, nutrientStores(), { processor("digester", "normal") },
				100.0, 50.0).first, "hot-sink setup refused");
			hot.resumeSimulation();
			require(hot.advanceTicks(60), "hot-sink run failed");
			auto const hotSnap = hot.getStationSystemsSnapshot();
			require(hotSnap.microbialProcessors[0].status == core::StationTransferStatus::Rejected
				&& !hotSnap.microbialProcessors[0].temperatureEligible
				&& hotSnap.microbialProcessors[0].feedConsumedKg == 0.0
				&& hotSnap.microbialProcessors[0].lastPowerSuppliedKW == 0.0
				&& hotSnap.microbialProcessors[0].blockReason.find("cooling is not qualified") != std::string::npos,
				"an above-window sink temperature did not refuse the processor whole");
		}
		// Finite destinations backpressure the whole stage: a full
		// nutrient store refuses the processor with a named reason, and a
		// small recovered-water store caps the digestion once its headroom
		// is gone - the total processed never exceeds what the tightest
		// destination can hold.
		{
			core::World world("NutrientFullStore", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = nutrientStores();
			for (auto& store : stores)
				if (store.id == "nutrients") { store.capacity = 0.0005; store.startingQualified = 0.0005; }
			require(nutrientConfigure(world, stores, { processor("digester", "normal") },
				100.0).first, "full-nutrient-store setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "full-nutrient-store run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.microbialProcessors[0].status == core::StationTransferStatus::Rejected
				&& snap.microbialProcessors[0].feedConsumedKg == 0.0
				&& snap.microbialProcessors[0].blockReason.find("nutrient store") != std::string::npos,
				"a full nutrient store did not refuse the processor with a named reason");
			require(findStore(snap, "nutrients").currentQualified <= 0.0005 + 1e-9,
				"a blocked destination was overfilled");
			core::World filling("NutrientWaterFills", 2, 2);
			filling.pauseSimulation(); filling.finishBuild();
			auto fillingStores = nutrientStores();
			for (auto& store : fillingStores)
				if (store.id == "recoveredWater") store.capacity = 0.0006;
			require(nutrientConfigure(filling, fillingStores, { processor("digester", "normal") },
				100.0).first, "filling-recovered-water setup refused");
			filling.resumeSimulation();
			require(filling.advanceTicks(180), "filling-recovered-water run failed");
			auto const fillSnap = filling.getStationSystemsSnapshot();
			require(approx(findStore(fillSnap, "recoveredWater").currentQualified, 0.0006),
				"the recovered water store did not fill to capacity");
			require(fillSnap.microbialProcessors[0].limiter.find("recovered water store") != std::string::npos
				|| fillSnap.microbialProcessors[0].blockReason.find("recovered water store") != std::string::npos,
				"the filled recovered-water store did not become the named limiter");
			// The whole run digested at most the water capacity's worth.
			require(fillSnap.microbialProcessors[0].feedConsumedKg
				<= 0.0006 / recoveredWaterPerKgFeed + 1e-9,
				"the processor digested past the recovered-water store's capacity");
		}
		// Protected reserves: the feed is drawn only above its protected
		// reserve, so a protected waste reserve is never digested away -
		// the run stops at the reserve line, not through it.
		{
			core::World world("NutrientReserve", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = nutrientStores();
			stores[0].startingQualified = 1.0;
			stores[0].protectedReserve = 0.99;
			require(nutrientConfigure(world, stores, { processor("digester", "normal") },
				100.0).first, "protected-reserve setup refused");
			world.resumeSimulation();
			// 0.01 kg of drawable feed at 0.001 kg/s lasts 10 s = 600
			// ticks; run past the exhaustion.
			require(world.advanceTicks(660), "protected-reserve run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(findStore(snap, "waste").currentQualified >= 0.99 - 1e-9,
				"the processor drew into the protected waste reserve");
			require(closeTo(snap.microbialProcessors[0].feedConsumedKg, 0.01, 0.02),
				"the digestion did not stop at the drawable feed line");
			require(snap.microbialProcessors[0].limiter.find("feed store") != std::string::npos
				|| snap.microbialProcessors[0].blockReason.find("feed store") != std::string::npos,
				"the reserve-capped processor did not name the feed store");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Disabled stage: a zero rated rate is idle, not broken - no
		// flows, no power draw, and the disabled flag is set.
		{
			core::World world("NutrientDisabled", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto offProcessor = processor("digester", "normal");
			offProcessor.rateKgPerS = 0.0;
			require(nutrientConfigure(world, nutrientStores(), { offProcessor }, 100.0).first,
				"disabled-stage setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "disabled-stage run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.microbialProcessors[0].disabled
				&& snap.microbialProcessors[0].status == core::StationTransferStatus::Idle
				&& snap.microbialProcessors[0].feedConsumedKg == 0.0
				&& snap.microbialProcessors[0].lastPowerSuppliedKW == 0.0,
				"a zero-rate processor is not an idle, zero-flow stage");
		}
	}

	void microbialConditionRecovery(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::max(1e-12, std::abs(expected));
		};
		// The #408 test biology: deliberately exaggerated provisional
		// per-hour coefficients so hour-scale degradation and recovery are
		// observable within short headless runs. Starvation 0.1/s, media
		// exhaustion 0.2/s, cold stress 0.05/s, heat stress 0.4/s,
		// recovery 0.02/s, replacement stock 10 kg per condition point.
		core::ResourceCompositionSettings feedComposition;
		feedComposition.moisture = 0.9;
		feedComposition.nutrientNitrogen = 0.05;
		feedComposition.atmosphericNitrogen = 0.02;
		feedComposition.phosphorus = 0.02;
		feedComposition.potassium = 0.02;
		feedComposition.otherMinerals = 0.03;
		feedComposition.energyKJPerKgDry = 8000.0;
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
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = 10000.0;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto processor = [](std::string const& id)
		{
			core::MicrobialProcessorSettings settings;
			settings.id = id;
			settings.hazardClass = "normal";
			settings.feedStoreId = "waste";
			settings.oxygenStoreId = "oxygen";
			settings.waterStoreId = "processWater";
			settings.consumableStoreId = "media";
			settings.nutrientStoreId = "nutrients";
			settings.recoveredWaterStoreId = "recoveredWater";
			settings.co2StoreId = "co2";
			settings.residueStoreId = "residue";
			settings.thermalClass = "low";
			settings.heatSinkId = "sink";
			settings.rateKgPerS = 0.001;
			settings.specificEnergyKWhPerKgFeed = 2.0;
			settings.oxygenPerKgFeedKg = 0.8;
			settings.processWaterPerKgFeedKg = 0.5;
			settings.consumablePerKgFeed = 0.01;
			settings.co2PerKgFeedKg = 0.3;
			settings.nitrogenRecovery = 0.8;
			settings.phosphorusRecovery = 0.8;
			settings.potassiumRecovery = 0.8;
			settings.mineralsRecovery = 0.7;
			settings.waterRecovery = 0.7;
			settings.reactionHeatKJPerKgFeed = 600.0;
			settings.minOperatingTemperatureC = 15.0;
			settings.maxSinkTemperatureC = 45.0;
			settings.starvationDegradePerHour = 360.0;
			settings.consumableExhaustionDegradePerHour = 720.0;
			settings.coldStressDegradePerHour = 180.0;
			settings.heatStressDegradePerHour = 1440.0;
			settings.recoveryPerHour = 72.0;
			return settings;
		};
		auto conditionStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			auto feed = plainStore("waste", 100.0, 10.0);
			feed.composition = feedComposition;
			stores.push_back(feed);
			stores.push_back(plainStore("oxygen", 100.0, 10.0));
			stores.push_back(plainStore("processWater", 100.0, 10.0));
			stores.push_back(plainStore("media", 100.0, 10.0));
			stores.push_back(plainStore("nutrients", 100.0, 0.0));
			stores.push_back(plainStore("recoveredWater", 100.0, 0.0));
			stores.push_back(plainStore("co2", 100.0, 0.0));
			stores.push_back(plainStore("residue", 100.0, 0.0));
			return stores;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto configure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			core::MicrobialProcessorSettings unit,
			double sinkStartingC = 20.0,
			std::vector<core::ResourceDeliverySettings> deliveries = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.thermalReservoirs = { reservoir("sink", "low", sinkStartingC) };
			settings.microbialProcessors = { std::move(unit) };
			settings.deliveries = std::move(deliveries);
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 100.0;
			settings.solarArrays = { field };
			return world.configureStationSystems(settings, false, &diagnostic);
		};
		auto conserve = [](core::StationSystemsSnapshot const& snap)
		{
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
			for (auto const& balance : snap.constituentBalances)
				require(std::abs(balance.residual) <= 2e-6,
					("constituent boundary residual for '" + balance.constituent + "' not closed").c_str());
		};
		// Benign operation leaves the culture at full condition: no
		// stressor acts, no degradation is applied, and the snapshot
		// reports the benign state.
		{
			core::World world("ConditionBenign", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(configure(world, conditionStores(), processor("digester")),
				"benign-condition setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "benign-condition run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& unit = snap.microbialProcessors[0];
			require(approx(unit.condition, 1.0) && unit.lastStressors.empty()
				&& !unit.recovering && unit.lastConditionDelta == 0.0
				&& unit.stockConsumedUnits == 0.0,
				"benign operation did not leave the culture at full, unstressed condition");
			conserve(snap);
		}
		// Starvation degrades the culture once the feed is gone: the
		// stressor is named, the condition falls at the declared rate,
		// and nothing digests or moves past the exhausted feed.
		{
			core::World world("ConditionStarvation", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = conditionStores();
			// 0.003 kg of feed lasts 3 s at the rated 0.001 kg/s.
			stores[0].startingQualified = 0.003;
			require(configure(world, stores, processor("digester")),
				"starvation setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(180), "starvation digest phase failed");
			auto const digestSnap = world.getStationSystemsSnapshot();
			require(approx(digestSnap.microbialProcessors[0].condition, 1.0)
				&& approx(digestSnap.microbialProcessors[0].feedConsumedKg, 0.003),
				"the culture did not digest the available feed at full condition");
			require(world.advanceTicks(120), "starvation degrade phase failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& unit = snap.microbialProcessors[0];
			// 2 s starved at 0.1 condition/s -> 0.8, within one tick of
			// boundary rounding.
			require(closeTo(unit.condition, 0.8, 0.01)
				&& unit.lastStressors == "starvation"
				&& !unit.recovering,
				"starvation did not degrade the culture at the declared rate with a named stressor");
			require(approx(unit.feedConsumedKg, 0.003)
				&& findStore(snap, "waste").currentQualified <= 1e-9
				&& approx(findStore(snap, "oxygen").currentQualified,
					10.0 - 0.003 * 0.8),
				"the starved processor kept consuming or fabricated material");
			conserve(snap);
		}
		// Condition throttles actual processing: a culture starting at
		// half condition drives only half the rated feed while every
		// ledger still closes - reduced performance never breaks
		// conservation, and the limiter names the condition.
		{
			core::World world("ConditionThrottle", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto unit = processor("digester");
			unit.initialCondition = 0.5;
			unit.recoveryPerHour = 0.0; // pure throttle: no recovery in this world
			require(configure(world, conditionStores(), unit), "throttle setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "throttle run failed");
			auto const elapsed = 60.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.microbialProcessors[0];
			require(approx(live.condition, 0.5)
				&& approx(live.scale, 0.5)
				&& approx(live.feedConsumedKg, 0.5 * 0.001 * elapsed),
				"half condition did not throttle the digestion to half the rated feed");
			require(live.status == core::StationTransferStatus::Partial
				&& live.limiter.find("culture condition degraded") != std::string::npos,
				"the condition throttle was not reported as the limiter");
			// nutrientPerKgFeed = 0.1 dry x (0.05+0.02+0.02)x0.8 + 0.03x0.7 = 0.0093
			require(approx(live.nutrientProducedKg,
				0.5 * 0.001 * elapsed * 0.0093),
				"the throttled recovery did not scale with the digestion");
			conserve(snap);
			bool sawConditionEvent = false;
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find("culture condition degraded") != std::string::npos)
					sawConditionEvent = true;
			require(sawConditionEvent,
				"the condition throttle never surfaced through the event seam");
		}
		// Recovery is bounded and stock-funded: a damaged culture recovers
		// only at the declared rate, each recovered condition point
		// consumes the configured stock above nothing for free, and when
		// the stock runs out the condition stops climbing - never an
		// instant restore.
		{
			core::World world("ConditionStockRecovery", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = conditionStores();
			stores.push_back(plainStore("stock", 10.0, 1.0));
			auto unit = processor("digester");
			unit.initialCondition = 0.5;
			unit.stockStoreId = "stock";
			unit.stockUnitsPerConditionPoint = 10.0;
			require(configure(world, stores, unit), "stock-recovery setup refused");
			world.resumeSimulation();
			// 1 s of recovery: 0.02 points at 10 kg/point = 0.2 kg stock.
			require(world.advanceTicks(60), "stock-recovery first phase failed");
			auto const first = world.getStationSystemsSnapshot();
			require(closeTo(first.microbialProcessors[0].condition, 0.52, 0.01)
				&& first.microbialProcessors[0].recovering
				&& closeTo(findStore(first, "stock").currentQualified, 0.8, 0.01),
				"recovery did not proceed at the declared rate funded by the declared stock");
			require(first.microbialProcessors[0].condition < 0.99,
				"recovery restored capacity instantly instead of being rate-bounded");
			// 1.0 kg of stock buys at most 0.1 condition points: the
			// culture caps at 0.6 once the stock is gone.
			require(world.advanceTicks(360), "stock-recovery second phase failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(closeTo(snap.microbialProcessors[0].condition, 0.6, 0.01)
				&& findStore(snap, "stock").currentQualified <= 1e-9
				&& closeTo(snap.microbialProcessors[0].stockConsumedUnits, 1.0, 0.01)
				&& snap.microbialProcessors[0].lastStockConsumedUnits < 1e-9,
				"recovery did not stop when the replacement stock was exhausted");
			conserve(snap);
		}
		// Temperature stress degrades the culture on both sides of the
		// window even though the stage runs nowhere: the cold excursion
		// costs cold-stress points, the hot excursion costs heat-stress
		// points, and neither spends feed.
		{
			core::World cold("ConditionColdStress", 2, 2);
			cold.pauseSimulation(); cold.finishBuild();
			require(configure(cold, conditionStores(), processor("digester"), 10.0),
				"cold-stress setup refused");
			cold.resumeSimulation();
			require(cold.advanceTicks(120), "cold-stress run failed");
			auto const coldSnap = cold.getStationSystemsSnapshot();
			require(closeTo(coldSnap.microbialProcessors[0].condition, 0.9, 0.01)
				&& coldSnap.microbialProcessors[0].lastStressors == "cold stress"
				&& coldSnap.microbialProcessors[0].feedConsumedKg == 0.0,
				"the cold excursion did not degrade the culture at the declared rate");
			conserve(coldSnap);
			core::World hot("ConditionHeatStress", 2, 2);
			hot.pauseSimulation(); hot.finishBuild();
			require(configure(hot, conditionStores(), processor("digester"), 50.0),
				"heat-stress setup refused");
			hot.resumeSimulation();
			require(hot.advanceTicks(120), "heat-stress run failed");
			auto const hotSnap = hot.getStationSystemsSnapshot();
			require(closeTo(hotSnap.microbialProcessors[0].condition, 0.2, 0.01)
				&& hotSnap.microbialProcessors[0].lastStressors == "heat stress"
				&& hotSnap.microbialProcessors[0].feedConsumedKg == 0.0,
				"the hot excursion did not degrade the culture at the declared rate");
			conserve(hotSnap);
		}
		// Consumable exhaustion degrades the culture through the named
		// rule while the stage is refused for want of medium.
		{
			core::World world("ConditionMediaOut", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = conditionStores();
			stores[3].startingQualified = 0.0; // media store empty
			require(configure(world, stores, processor("digester")),
				"media-exhaustion setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "media-exhaustion run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(closeTo(snap.microbialProcessors[0].condition, 0.8, 0.01)
				&& snap.microbialProcessors[0].lastStressors == "consumable exhaustion"
				&& snap.microbialProcessors[0].feedConsumedKg == 0.0,
				"media exhaustion did not degrade the culture at the declared rate");
			conserve(snap);
		}
		// The full adversity/recovery cycle: digest at full condition,
		// starve on the gap between feedings, then recover through the
		// declared time-bound rate once the resupply lands - gradual,
		// accounted, and conserved through every phase.
		{
			core::World world("ConditionCycle", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = conditionStores();
			stores[0].startingQualified = 0.003; // 3 s of feed
			core::ResourceDeliverySettings resupply;
			resupply.id = "feeddrop";
			resupply.storeId = "waste";
			resupply.unit = "kg";
			resupply.quantity = 5.0;
			resupply.firstOccurrenceSeconds = 6.0;
			require(configure(world, stores, processor("digester"), 20.0, { resupply }),
				"adversity-cycle setup refused");
			world.resumeSimulation();
			// Through the starvation gap: 3 s digesting, then degrading
			// at 0.1/s until the 6 s resupply. Sample mid-gap while the
			// culture is definitely unfed.
			require(world.advanceTicks(300), "adversity-cycle starve phase failed");
			auto const starved = world.getStationSystemsSnapshot();
			require(closeTo(starved.microbialProcessors[0].condition, 0.8, 0.01)
				&& starved.microbialProcessors[0].lastStressors == "starvation",
				"the starvation gap did not degrade the culture as declared");
			conserve(starved);
			// After the resupply the culture is fed again: recovery
			// advances at 0.02/s with no stock configured - time-bound
			// recovery alone. 5 s of recovery adds at most 0.1.
			require(world.advanceTicks(360), "adversity-cycle recovery phase failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(closeTo(snap.microbialProcessors[0].condition, 0.8, 0.01)
				&& snap.microbialProcessors[0].lastStressors.empty()
				&& snap.microbialProcessors[0].recovering
				&& snap.microbialProcessors[0].stockConsumedUnits == 0.0,
				"the fed culture did not recover through the declared bounded rate");
			require(snap.microbialProcessors[0].feedConsumedKg > 0.003,
				"the culture did not resume digesting after the resupply");
			conserve(snap);
		}
		// Zero-coefficient stressors still bar recovery (#570): a zero
		// degradation coefficient disables only that rule's degradation
		// contribution, not the benign precondition for recovery. With
		// each stressor active at a zero rate the culture neither degrades
		// nor recovers - with or without replacement stock - and the
		// snapshot still names the stressor. The controls show the same
		// all-zero configuration still recovers under benign conditions
		// and still degrades at the rate of any nonzero co-acting rule.
		{
			auto zeroRate = [&]()
			{
				auto unit = processor("digester");
				unit.initialCondition = 0.5;
				unit.starvationDegradePerHour = 0.0;
				unit.consumableExhaustionDegradePerHour = 0.0;
				unit.coldStressDegradePerHour = 0.0;
				unit.heatStressDegradePerHour = 0.0;
				return unit;
			};
			auto zeroRateScenario = [&](char const* name,
				std::vector<core::ResourceStoreSettings> stores,
				std::string const& expectedStressor,
				double sinkStartingC, bool withStock)
			{
				core::World world(name, 2, 2);
				world.pauseSimulation(); world.finishBuild();
				if (withStock)
					stores.push_back(plainStore("stock", 10.0, 1.0));
				auto unit = zeroRate();
				if (withStock)
				{
					unit.stockStoreId = "stock";
					unit.stockUnitsPerConditionPoint = 10.0;
				}
				require(configure(world, std::move(stores), std::move(unit), sinkStartingC),
					(std::string(name) + " setup refused").c_str());
				world.resumeSimulation();
				require(world.advanceTicks(120), (std::string(name) + " run failed").c_str());
				auto const snap = world.getStationSystemsSnapshot();
				auto const& unitSnap = snap.microbialProcessors[0];
				require(unitSnap.lastStressors == expectedStressor
					&& !unitSnap.recovering
					&& approx(unitSnap.condition, 0.5)
					&& unitSnap.lastConditionDelta == 0.0
					&& unitSnap.stockConsumedUnits == 0.0
					&& unitSnap.lastStockConsumedUnits == 0.0,
					(std::string("zero-rate ") + expectedStressor
						+ (withStock ? " with stock" : " without stock")
						+ " was not frozen at its initial condition").c_str());
				if (withStock)
					require(approx(findStore(snap, "stock").currentQualified, 1.0),
						(std::string("zero-rate ") + expectedStressor
							+ " drew replacement stock while the culture was barred from recovery")
							.c_str());
				conserve(snap);
			};
			// Starvation: empty feed store, zero starvation coefficient.
			{
				auto stores = conditionStores();
				stores[0].startingQualified = 0.0;
				zeroRateScenario("ZeroRateStarvingNoStock", stores, "starvation", 20.0, false);
				zeroRateScenario("ZeroRateStarvingWithStock", stores, "starvation", 20.0, true);
			}
			// Empty medium: zero consumable-exhaustion coefficient.
			{
				auto stores = conditionStores();
				stores[3].startingQualified = 0.0;
				zeroRateScenario("ZeroRateMediaOutNoStock", stores,
					"consumable exhaustion", 20.0, false);
				zeroRateScenario("ZeroRateMediaOutWithStock", stores,
					"consumable exhaustion", 20.0, true);
			}
			// Cold: sink below the minimum operating temperature with a
			// zero cold-stress coefficient.
			zeroRateScenario("ZeroRateColdNoStock", conditionStores(),
				"cold stress", 10.0, false);
			zeroRateScenario("ZeroRateColdWithStock", conditionStores(),
				"cold stress", 10.0, true);
			// Hot: sink at or above the maximum sink temperature with a
			// zero heat-stress coefficient.
			zeroRateScenario("ZeroRateHotNoStock", conditionStores(),
				"heat stress", 50.0, false);
			zeroRateScenario("ZeroRateHotWithStock", conditionStores(),
				"heat stress", 50.0, true);
			// Control: the same all-zero configuration under benign
			// conditions still recovers at the declared rate - zero
			// coefficients disable the degradation contributions, not
			// recovery itself.
			{
				core::World world("ZeroRateBenignRecovers", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				require(configure(world, conditionStores(), zeroRate()),
					"all-zero benign control setup refused");
				world.resumeSimulation();
				require(world.advanceTicks(60), "all-zero benign control run failed");
				auto const snap = world.getStationSystemsSnapshot();
				auto const& unitSnap = snap.microbialProcessors[0];
				// 1 s at 72/hour = 0.02 points on top of 0.5.
				require(closeTo(unitSnap.condition, 0.52, 0.01)
					&& unitSnap.lastStressors.empty() && unitSnap.recovering,
					"the all-zero benign culture did not recover as declared");
				conserve(snap);
			}
			// Control: a zero starvation coefficient does not mask a
			// co-acting nonzero rule - the culture still degrades at the
			// nonzero rate and never recovers while that stressor acts.
			{
				core::World world("ZeroRateMixedDegrades", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				auto stores = conditionStores();
				stores[3].startingQualified = 0.0; // media empty
				auto unit = zeroRate();
				unit.consumableExhaustionDegradePerHour = 720.0; // nonzero co-actor
				require(configure(world, std::move(stores), std::move(unit)),
					"mixed zero/nonzero control setup refused");
				world.resumeSimulation();
				require(world.advanceTicks(60), "mixed zero/nonzero control run failed");
				auto const snap = world.getStationSystemsSnapshot();
				auto const& unitSnap = snap.microbialProcessors[0];
				// 1 s at 720/hour = 0.2 points off 0.5.
				require(closeTo(unitSnap.condition, 0.3, 0.01)
					&& unitSnap.lastStressors == "consumable exhaustion"
					&& !unitSnap.recovering,
					"the zero starvation coefficient masked the nonzero exhaustion rule");
				conserve(snap);
			}
		}
	}
}

void registerStationRecoveryChecks(std::vector<smoke::Check>& checks)
{
	checks.insert(checks.end(), {
		{"carbonRecovery", carbonRecovery},
		{"nutrientRecovery", nutrientRecovery},
		{"microbialConditionRecovery", microbialConditionRecovery}});
}
