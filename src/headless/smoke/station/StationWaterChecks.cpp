#include "StationWaterChecks.h"
#include "StationRig.h"
#include "core/StationConfigurationDocument.h"
#include <limits>

namespace
{
	using namespace smoke::station;

	// #400: compartment atmospheres - analytic ideal-gas mixtures,
	// leakage, zero/low inventory, vapour constraints, compartment
	// isolation, deterministic scheduled airlock losses with the
	// recovered-versus-lost split, and heat-only thermal-reservoir
	// coupling. All checks run through the World tick/snapshot/event/
	// diagnostic seam and observable behaviour only.
	// #401: crew supplies turn into balanced metabolic returns.
	void crews(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto const approx = [](double actual, double expected, double tolerance = 1e-9)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		// Authored compositions (dry-mass basis, #391) chosen so the
		// declared crew coefficients close: consumed mass (food + water
		// + O2) exceeds emitted mass (CO2 + moisture + urine + organic
		// waste) with an explicit retained remainder; every returned
		// constituent is funded by the consumed food and water; and the
		// declared metabolic heat sits below the food energy minus the
		// energy still bound in the waste streams.
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
		auto store = [](std::string const& id, std::string const& unit,
			double capacity, double starting,
			std::optional<core::ResourceCompositionSettings> composition)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = unit;
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.composition = std::move(composition);
			return settings;
		};
		auto compartment = []()
		{
			core::AtmosphereSettings settings;
			settings.id = "habitat";
			settings.volumeM3 = 100.0;
			settings.startingOxygenKg = 20.0;
			settings.startingNitrogenKg = 80.0;
			settings.startingTemperatureC = 20.0;
			settings.thermalCapacityKJPerK = 10000.0;
			settings.minPressureKPa = 1.0;
			settings.maxPressureKPa = 200.0;
			return settings;
		};
		// Per-crew full-activity rates: consumed 0.004 kg/s, emitted
		// 0.0033 kg/s, retained 0.0007 kg/s; released food energy
		// 2.3625 kJ/s against 0.1 kW declared metabolic heat.
		auto crew = [](int count, double activityLevel)
		{
			core::CrewSettings settings;
			settings.id = "crew";
			settings.atmosphereId = "habitat";
			settings.crewCount = count;
			settings.activityProfile.constantLevel = activityLevel;
			settings.foodStoreId = "food";
			settings.waterStoreId = "water";
			settings.urineStoreId = "urine";
			settings.organicWasteStoreId = "organicWaste";
			settings.foodPerCrewPerS = 0.001;
			settings.waterPerCrewPerS = 0.002;
			settings.oxygenPerCrewKgPerS = 0.001;
			settings.co2PerCrewKgPerS = 0.0011;
			settings.moisturePerCrewKgPerS = 0.0005;
			settings.urinePerCrewPerS = 0.0015;
			settings.organicWastePerCrewPerS = 0.0002;
			settings.metabolicHeatPerCrewKW = 0.1;
			return settings;
		};
		auto fullStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(store("food", "kg", 100.0, 50.0, foodComposition));
			stores.push_back(store("water", "kg", 100.0, 50.0, waterComposition));
			stores.push_back(store("urine", "kg", 100.0, 0.0, urineComposition));
			stores.push_back(store("organicWaste", "kg", 100.0, 0.0, wasteComposition));
			return stores;
		};
		auto crewConfigure = [](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			core::AtmosphereSettings habitat,
			std::vector<core::CrewSettings> crewSettings)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { habitat };
			settings.crews = std::move(crewSettings);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// #743: optional greywater is a funded, uniformly scaled return,
		// not material created by labelling the destination composition.
		{
			struct SourceReset { ~SourceReset() { core::setStationSystemsSourceLoader({}); } } reset;
			core::setStationSystemsSourceLoader([] {
				auto source = core::bundledStationSystemsSource();
				auto const at = source.find("        self.ticks = self.ticks + 1");
				require(at != std::string::npos, "greywater Lua insertion point missing");
				source.insert(at, R"lua(
        local c = self.crews[1]
        assert(math.abs((c.greywaterConstituents.nutrientNitrogen or 0) - c.greywaterKg * 0.001) < 1e-12)
        assert(math.abs((c.lastGreywaterConstituents.dryOrganics or 0) - c.lastGreywaterKg * 0.01) < 1e-12)
)lua");
				return source;
			});
			auto stores = fullStores();
			core::ResourceCompositionSettings pure; pure.moisture = 0.999;
			stores.push_back(store("greywater", "kg", 100, 0, pure));
			stores.back().materialId = "water";
			auto c = crew(2, 1);
			c.greywaterStoreId = "greywater";
			c.greywaterKgPerCrewDay = 8.64; // 0.0001 kg/s from retained mass
			c.greywaterLoadKgPerCrewDay = {{"dryOrganics", 0.0864}, {"nutrientNitrogen", 0.00864}};
			core::World world("Greywater", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto [ok, diagnostic] = crewConfigure(world, stores, compartment(), {c});
			require(ok, diagnostic.c_str());
			world.resumeSimulation(); require(smoke::station::advanceSeconds(world, 1.0), "greywater tick failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.healthy, "greywater became unhealthy");
			require(!snap.forecast.available && snap.forecast.unavailableReason.find("greywater") != std::string::npos,
				"forecast silently ignored greywater backpressure");
			auto const& actual = snap.crews.at(0);
			require(approx(actual.greywaterKg, 2 * 60 * dt * 0.0001), "wrong greywater return");
			require(approx(actual.greywaterConstituents.at("dryOrganics"), actual.greywaterKg * 0.01), "greywater organic load missing");
			require(approx(actual.greywaterConstituents.at("nutrientNitrogen"), actual.greywaterKg * 0.001), "greywater N load missing");
			require(approx(snap.stores.back().actualConstituents.at("dryOrganics"), actual.greywaterKg * 0.01), "greywater load absent from store ledger");
			for (auto const& balance : snap.constituentBalances)
				require(approx(balance.residual, 0), "greywater constituent boundary did not close");
			core::StationConfigurationDocument document, restored;
			document.settings = world.getStationSystemsSettings();
			auto const parsed = core::parseStationConfiguration(core::serializeStationConfiguration(document), restored, &diagnostic);
			require(parsed && restored.settings == document.settings, "greywater declaration lost on save/load");
			core::StationSystemsSettings bad;
			bad.enabled = true; bad.stores = stores; bad.atmospheres = {compartment()}; bad.crews = {c};
			for (double invalid : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 1000.0})
			{
				bad.crews[0].greywaterLoadKgPerCrewDay["dryOrganics"] = invalid;
				require(!core::validateStationSystemsSettings(bad, diagnostic), "invalid greywater load accepted");
			}
			bad.crews = {c}; bad.crews[0].greywaterStoreId = "water";
			require(!core::validateStationSystemsSettings(bad, diagnostic), "aliased greywater accepted");
			stores.back().capacity = 100;
			stores.back().startingQualified = 100;
			core::World full("GreywaterFull", 2, 2); full.pauseSimulation(); full.finishBuild();
			auto [fullOk, fullDiagnostic] = crewConfigure(full, stores, compartment(), {c});
			require(fullOk, fullDiagnostic.c_str());
			full.resumeSimulation(); require(smoke::station::advanceSeconds(full, 0.0166666666667), "full greywater tick failed");
			auto const blocked = full.getStationSystemsSnapshot().crews.at(0);
			require(blocked.metabolicScale == 0 && blocked.lastFoodActual == 0 && blocked.greywaterKg == 0
				&& blocked.limiter.find("greywater") != std::string::npos, "greywater backpressure did not throttle whole crew");
		}
		// Refused configurations: every malformed crew contract is
		// rejected before anything runs, and none of the refusals
		// mutates the World.
		{
			core::World bad("CrewBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = fullStores();
			settings.atmospheres = { compartment() };
			std::string diagnostic;
			settings.crews = { crew(2, 1.0) };
			settings.crews[0].atmosphereId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown atmosphere compartment") != std::string::npos,
				"a crew breathing in an unknown compartment was accepted");
			settings.crews = { crew(2, 1.0) };
			settings.crews[0].foodStoreId = "pantry";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food store 'pantry' does not exist") != std::string::npos,
				"a crew fed from an unknown store was accepted");
			settings.crews = { crew(2, 1.0) };
			settings.stores = fullStores();
			settings.stores[1].unit = "L";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("mass-denominated") != std::string::npos,
				"a crew stream in a non-mass unit was accepted");
			settings.crews = { crew(2, 1.0) };
			settings.stores = fullStores();
			settings.stores[2].composition.reset();
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("no declared composition") != std::string::npos,
				"a crew returning to a plain (non-compound) store was accepted");
			settings.crews = { crew(-1, 1.0) };
			settings.stores = fullStores();
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("crew count must not be negative") != std::string::npos,
				"a negative crew count was accepted");
			// Mass creation: CO2 out pushes emitted mass above consumed.
			settings.crews = { crew(2, 1.0) };
			settings.crews[0].co2PerCrewKgPerS = 0.002;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("may not create mass") != std::string::npos,
				"returns exceeding consumption were accepted");
			// Element creation: urine carries more nitrogen than the food
			// can fund.
			settings.crews = { crew(2, 1.0) };
			settings.stores = fullStores();
			settings.stores[2].composition->nutrientNitrogen = 0.5;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("nutrientNitrogen") != std::string::npos
				&& diagnostic.find("element balance may not be created") != std::string::npos,
				"a urine nitrogen load the food cannot fund was accepted");
			// Energy creation: declared heat above the released food energy.
			settings.crews = { crew(2, 1.0) };
			settings.stores = fullStores();
			settings.crews[0].metabolicHeatPerCrewKW = 5.0;
			auto const heatOk = core::validateStationSystemsSettings(settings, diagnostic);
			require(!heatOk
				&& diagnostic.find("heat may not be created from nothing") != std::string::npos,
				("metabolic heat beyond the funded food energy was accepted (ok="
				+ std::to_string(heatOk) + ", diag=" + diagnostic + ")").c_str());
			settings.crews = { crew(2, 1.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic),
				"an activity level above one was accepted");
			std::vector<core::CrewSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationCrews; ++index)
				tooMany.push_back(crew(1, 1.0));
			settings.crews = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared crew budget was accepted");
			// Cross-class crew wiring is accepted (#751): hazardous food
			// metabolised by a normal-class crew into normal returns, a
			// hazardous drinking store and a hazardous return store all
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused crew configuration mutated the World");
		}
		// Aliased crew stream roles (#600), the same rule #585 carries for
		// the electrolyzer and the fuel cell and #599 carries for the
		// carbon-recovery machines: every pair of the crew's four
		// physically distinct stream roles - food, water, urine and
		// organic waste - must name a distinct store. The tick sized each
		// draw and return against the store's inventory or headroom
		// measured from the same starting state, then booked them
		// sequentially, so one store named for two roles had the same
		// inventory spent twice: aliased inputs left the second draw with
		// nothing while the returns still ran at the original scale,
		// driving retained body mass negative and pausing the World at
		// tick zero; aliased outputs clipped the second return after both
		// inputs were consumed, silently changing the metabolic balance
		// the author sized. Every pair is refused at configuration,
		// whatever the fill levels, and the World is never mutated.
		{
			auto const alias = "chemically distinct streams may not share one store";
			struct Alias
			{
				std::string left;
				std::string right;
			};
			// Nearly full outputs, nearly empty inputs: the refusal is about
			// the roles, not the fill levels, so it must fire at any level.
			auto const tightStores = [&]()
			{
				std::vector<core::ResourceStoreSettings> stores;
				stores.push_back(store("food", "kg", 100.0, 0.001, foodComposition));
				stores.push_back(store("water", "kg", 100.0, 0.001, waterComposition));
				stores.push_back(store("urine", "kg", 1.0, 0.99, urineComposition));
				stores.push_back(store("organicWaste", "kg", 1.0, 0.99, wasteComposition));
				return stores;
			}();
			core::World tight("CrewAliasedRoles", 2, 2);
			tight.pauseSimulation(); tight.finishBuild();
			std::string diagnostic;
			// The role names below are the fixture store ids themselves, so
			// aliasing a pair means pointing the right-hand role at the
			// left-hand role's store.
			auto setRole = [](core::CrewSettings& unit,
				std::string const& role, std::string const& id)
			{
				if (role == "food") unit.foodStoreId = id;
				else if (role == "water") unit.waterStoreId = id;
				else if (role == "urine") unit.urineStoreId = id;
				else unit.organicWasteStoreId = id;
			};
			// All six pairs of the crew's four stream roles: input/input,
			// input/output and output/output aliases.
			std::vector<Alias> crewAliases = {
				{"food", "water"}, {"food", "urine"}, {"food", "organicWaste"},
				{"water", "urine"}, {"water", "organicWaste"},
				{"urine", "organicWaste"},
			};
			for (auto const& pair : crewAliases)
			{
				core::StationSystemsSettings settings;
				settings.enabled = true;
				settings.stores = tightStores;
				settings.atmospheres = { compartment() };
				settings.crews = { crew(1, 1.0) };
				setRole(settings.crews[0], pair.right, pair.left);
				require(!core::validateStationSystemsSettings(settings, diagnostic)
					&& diagnostic.find(alias) != std::string::npos,
					("a crew aliasing its '" + pair.left + "' and '" + pair.right
						+ "' stream roles was accepted").c_str());
			}
			require(!tight.getStationSystemsSnapshot().enabled
				&& tight.getSimulationTick() == 0,
				"the refused aliased crew-role probes mutated the World");
			// The reported reproductions through the public configure/tick/
			// snapshot seam, with the ticket's own settings. Case 1 aliased
			// food and drinking water onto one store holding 0.01 kg: the
			// configuration was admitted, then the first tick failed and
			// paused the World at tick zero with "retained body mass is
			// negative". Case 2 aliased urine and organic waste onto one
			// 1 kg store holding 0.99 kg: one tick consumed 0.01 kg from
			// each input while adding only 0.01 kg total to the shared
			// output, breaking the authored common-scale contract. Both are
			// now refused at configure, so the broken tick never runs and
			// the World is never touched.
			{
				core::World world("CrewAliasRepro", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				core::StationSystemsSettings inputs;
				inputs.enabled = true;
				inputs.stores = {
					store("food", "kg", 100.0, 0.01, foodComposition),
					store("water", "kg", 100.0, 0.0, waterComposition),
					store("waste", "kg", 1.0, 0.0, wasteComposition) };
				inputs.atmospheres = { compartment() };
				core::CrewSettings aliasedInputs = crew(1, 1.0);
				aliasedInputs.waterStoreId = "food";
				aliasedInputs.urineStoreId = "water";
				aliasedInputs.organicWasteStoreId = "waste";
				aliasedInputs.foodPerCrewPerS = 1.0;
				aliasedInputs.waterPerCrewPerS = 1.0;
				aliasedInputs.urinePerCrewPerS = 1.0;
				aliasedInputs.organicWastePerCrewPerS = 1.0;
				inputs.crews = { aliasedInputs };
				require(!world.configureStationSystems(inputs, true, &diagnostic)
					&& diagnostic.find(alias) != std::string::npos,
					("the reproduction's aliased food/water crew was accepted: "
						+ diagnostic).c_str());
				require(!world.getStationSystemsSnapshot().enabled
					&& world.getSimulationTick() == 0
					&& world.getStationSystemsDiagnostics().empty(),
					"the refused aliased-input reproduction mutated the World");
				// Case 2: the aliased-outputs variant of the reproduction.
				core::StationSystemsSettings outputs;
				outputs.enabled = true;
				outputs.stores = {
					store("food", "kg", 100.0, 10.0, foodComposition),
					store("water", "kg", 100.0, 10.0, waterComposition),
					store("sharedWaste", "kg", 1.0, 0.99, urineComposition) };
				outputs.atmospheres = { compartment() };
				core::CrewSettings aliasedOutputs = crew(1, 1.0);
				aliasedOutputs.urineStoreId = "sharedWaste";
				aliasedOutputs.organicWasteStoreId = "sharedWaste";
				aliasedOutputs.foodPerCrewPerS = 1.0;
				aliasedOutputs.waterPerCrewPerS = 1.0;
				aliasedOutputs.urinePerCrewPerS = 1.0;
				aliasedOutputs.organicWastePerCrewPerS = 1.0;
				outputs.crews = { aliasedOutputs };
				require(!world.configureStationSystems(outputs, true, &diagnostic)
					&& diagnostic.find(alias) != std::string::npos,
					("the reproduction's aliased urine/organic-waste crew was accepted: "
						+ diagnostic).c_str());
				require(!world.getStationSystemsSnapshot().enabled
					&& world.getSimulationTick() == 0
					&& world.getStationSystemsDiagnostics().empty(),
					"the refused aliased-output reproduction mutated the World");
			}
			// Valid sharing between different crews is preserved: two crews
			// draw food from one store and return into one urine store,
			// booking sequentially against the live inventory, so the shared
			// supply is spent once, not twice. The actual per-crew draws and
			// returns, the shared-store depletion, the retained mass and the
			// constituent closure are asserted, not just nonnegative
			// inventory. The rule refuses aliases within a crew, never
			// sharing across crews.
			{
				core::World world("CrewSharedAcrossCrews", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				auto first = crew(2, 1.0);
				auto second = crew(1, 1.0);
				second.id = "crewB";
				require(crewConfigure(world, fullStores(), compartment(),
					{ first, second }).first,
					"two crews sharing the food, water, urine and waste stores were refused");
				world.resumeSimulation();
				require(smoke::station::advanceSeconds(world, 2.0), "the shared-store crew run failed");
				require(world.getStationSystemsDiagnostics().empty(),
					"the shared-store crews raised a runtime diagnostic");
				auto const elapsed = 120.0 * dt;
				auto const snap = world.getStationSystemsSnapshot();
				require(snap.healthy && snap.crews.size() == 2,
					"the shared-store crews are missing or unhealthy in the snapshot");
				auto const& a = snap.crews[0];
				auto const& b = snap.crews[1];
				require(approx(a.foodConsumed, 0.001 * 2.0 * elapsed)
					&& approx(b.foodConsumed, 0.001 * 1.0 * elapsed),
					"a crew sharing the food store did not draw its authored rate");
				// The shared stores gave up exactly the summed consumption -
				// no double-spend against the same starting inventory.
				require(approx(snap.stores[0].currentQualified,
					50.0 - a.foodConsumed - b.foodConsumed),
					"the shared food store was spent more than once");
				require(approx(snap.stores[1].currentQualified,
					50.0 - a.waterConsumed - b.waterConsumed),
					"the shared water store was spent more than once");
				require(approx(snap.stores[2].currentQualified,
					a.urineProduced + b.urineProduced),
					"the shared urine store did not receive exactly the summed returns");
				require(a.retainedKg > 0.0 && b.retainedKg > 0.0,
					"a sharing crew retained no body mass - the balance was altered");
				for (auto const& balance : snap.constituentBalances)
					require(std::abs(balance.residual) <= 1e-6,
						("constituent '" + balance.constituent
							+ "' ledger not closed").c_str());
			}
		}
		// Nominal metabolism: every pathway runs at the authored rate,
		// the compartment gains the crew's CO2, moisture and heat, the
		// waste stores receive the returns, and the crew ledger closes
		// over consumed = returned + retained with the element balances
		// intact.
		{
			core::World world("CrewNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(crewConfigure(world, fullStores(), compartment(), { crew(2, 1.0) }).first,
				"nominal crew refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 2.0), "nominal crew run failed");
			auto const elapsed = 120.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.crews.size() == 1, "crew missing from snapshot");
			auto const& live = snap.crews[0];
			require(live.status == core::StationTransferStatus::Full
				&& approx(live.metabolicScale, 1.0), "nominal crew is not fully supplied");
			require(approx(live.foodConsumed, 0.001 * 2.0 * elapsed), "food consumption mismatch");
			require(approx(live.waterConsumed, 0.002 * 2.0 * elapsed), "water consumption mismatch");
			require(approx(live.oxygenConsumedKg, 0.001 * 2.0 * elapsed), "oxygen consumption mismatch");
			require(approx(live.co2ProducedKg, 0.0011 * 2.0 * elapsed), "CO2 return mismatch");
			require(approx(live.moistureProducedKg, 0.0005 * 2.0 * elapsed), "moisture return mismatch");
			require(approx(live.urineProduced, 0.0015 * 2.0 * elapsed), "urine return mismatch");
			require(approx(live.organicWasteProduced, 0.0002 * 2.0 * elapsed), "organic waste return mismatch");
			require(approx(live.heatKJ, 0.1 * 2.0 * elapsed), "metabolic heat mismatch");
			require(approx(live.retainedKg, (0.004 - 0.0033) * 2.0 * elapsed),
				"retained body mass does not close over consumed minus returned");
			// The compartment books the crew's gas, moisture and heat.
			auto const& habitat = snap.atmospheres[0];
			require(approx(habitat.oxygenKg, 20.0 - 0.001 * 2.0 * elapsed),
				"the compartment did not supply the crew's oxygen");
			require(approx(habitat.co2ProducedKg, 0.0011 * 2.0 * elapsed),
				"the crew's CO2 is missing from the compartment ledger");
			require(approx(habitat.vapourAddedKg, 0.0005 * 2.0 * elapsed),
				"the crew's moisture is missing from the compartment ledger");
			require(approx(habitat.crewHeatEnergyKJ, 0.1 * 2.0 * elapsed),
				"the crew's metabolic heat is missing from the compartment ledger");
			require(habitat.temperatureC > 20.0, "crew heat did not warm the compartment");
			require(std::abs(habitat.residualKJ) < 1e-6, "compartment thermal ledger not closed");
			// The waste stores received the returns; the food and water
			// stores gave up exactly the consumption.
			auto const& urineStore = snap.stores[2];
			auto const& wasteStore = snap.stores[3];
			require(approx(urineStore.currentQualified, 0.0015 * 2.0 * elapsed),
				"the urine store did not receive the returns");
			require(approx(wasteStore.currentQualified, 0.0002 * 2.0 * elapsed),
				"the organic waste store did not receive the returns");
			require(approx(snap.stores[0].currentQualified, 50.0 - 0.001 * 2.0 * elapsed),
				"the food store gave up more or less than the crew consumed");
			// Element conservation: every constituent ledger closes with
			// no residual beyond tolerance.
			for (auto const& balance : snap.constituentBalances)
				require(std::abs(balance.residual) <= 1e-6,
					("constituent '" + balance.constituent + "' ledger not closed").c_str());
			// The crew's own closure: consumed = returned + retained.
			require(live.retainedKg > 0.0, "nominal metabolism retained nothing");
		}
		// Missing food: the uniform scale collapses to zero, so nothing
		// else is consumed and nothing is returned - no nominal waste
		// from material that is not there - and the shortfall is
		// explicit on every pathway.
		{
			core::World world("CrewNoFood", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = fullStores();
			stores[0].startingQualified = 0.0;
			require(crewConfigure(world, stores, compartment(), { crew(2, 1.0) }).first,
				"zero-food crew refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "zero-food run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.crews[0];
			require(live.status == core::StationTransferStatus::Rejected
				&& approx(live.metabolicScale, 0.0), "a crew with no food is not starved");
			require(live.lastFoodUnmet > 0.0 && live.lastFoodActual == 0.0,
				"the food shortfall is not visible as unmet demand");
			require(live.waterConsumed == 0.0 && live.oxygenConsumedKg == 0.0,
				"a starved crew still consumed water or oxygen");
			require(live.urineProduced == 0.0 && live.organicWasteProduced == 0.0
				&& live.co2ProducedKg == 0.0 && live.moistureProducedKg == 0.0
				&& live.heatKJ == 0.0,
				"absent food generated nominal waste or heat");
			require(sawEvent(world, "metabolic demand unmet: food store"),
				"the crew starvation transition was never announced");
		}
		// Partial supply: the scale lands on the scarcest input and
		// every pathway - consumption and return - throttles together
		// with it; requested stays visible above actual.
		{
			core::World world("CrewPartialWater", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = fullStores();
			// One tick of water demand for 2 crew is 0.004*dt; hold half.
			stores[1].startingQualified = 0.002 * dt;
			require(crewConfigure(world, stores, compartment(), { crew(2, 1.0) }).first,
				"partial-water crew refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 0.0166666666667), "partial-water tick failed");
			auto const first = world.getStationSystemsSnapshot();
			auto const& live = first.crews[0];
			require(approx(live.metabolicScale, 0.5, 1e-9),
				"the metabolic scale did not land on the half-full water store");
			require(approx(live.foodConsumed, 0.5 * 0.001 * 2.0 * dt),
				"food did not throttle with the water-limited scale");
			require(approx(live.oxygenConsumedKg, 0.5 * 0.001 * 2.0 * dt),
				"oxygen did not throttle with the water-limited scale");
			require(approx(live.urineProduced, 0.5 * 0.0015 * 2.0 * dt),
				"urine did not throttle with the water-limited scale");
			require(approx(live.heatKJ, 0.5 * 0.1 * 2.0 * dt),
				"heat did not throttle with the water-limited scale");
			require(live.lastWaterUnmet > 0.0 && live.lastFoodUnmet > 0.0,
				"partial supply left no visible unmet remainder");
			require(live.status == core::StationTransferStatus::Partial,
				"partial metabolism does not report a partial status");
			// The water is gone: the next tick runs at zero scale.
			smoke::station::advanceSeconds(world, 0.0166666666667);
			require(world.getStationSystemsSnapshot().crews[0].status
				== core::StationTransferStatus::Rejected,
				"an emptied water store did not starve the crew");
		}
		// Missing oxygen: the compartment cannot breathe for the crew, so
		// nothing runs - no food draw, no CO2, no heat - and the
		// compartment's own inventory is untouched by the refusal.
		{
			core::World world("CrewNoOxygen", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto habitat = compartment();
			habitat.startingOxygenKg = 0.0;
			require(crewConfigure(world, fullStores(), habitat, { crew(2, 1.0) }).first,
				"zero-oxygen crew refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "zero-oxygen run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.crews[0];
			require(live.status == core::StationTransferStatus::Rejected,
				"a crew with no oxygen is not starved");
			require(live.lastOxygenUnmetKg > 0.0, "the oxygen shortfall is not visible");
			require(live.foodConsumed == 0.0 && live.waterConsumed == 0.0,
				"a crew with no oxygen still consumed food or water");
			require(live.co2ProducedKg == 0.0 && live.heatKJ == 0.0,
				"a crew with no oxygen produced CO2 or heat from nothing");
			require(sawEvent(world, "oxygen inventory cannot supply the draw"),
				"the oxygen starvation limiter was never announced");
		}
		// A full waste tank throttles the metabolism: consumption cannot
		// run ahead of a destination that cannot hold the returns.
		{
			core::World world("CrewFullUrine", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = fullStores();
			stores[2].startingQualified = 100.0; // at capacity
			require(crewConfigure(world, stores, compartment(), { crew(2, 1.0) }).first,
				"full-urine-tank crew refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "full-urine-tank run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.crews[0];
			require(live.status == core::StationTransferStatus::Rejected,
				"a full waste tank did not throttle the crew");
			require(live.foodConsumed == 0.0 && live.urineProduced == 0.0,
				"consumption ran with nowhere for the returns to go");
			require(sawEvent(world, "urine store 'urine' is at capacity"),
				"the waste-headroom limiter was never announced");
		}
		// Activity scales every pathway: a half-activity crew eats,
		// breathes, works and excretes at half the nominal rates.
		{
			core::World world("CrewHalfActivity", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(crewConfigure(world, fullStores(), compartment(), { crew(2, 0.5) }).first,
				"half-activity crew refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 2.0), "half-activity run failed");
			auto const elapsed = 120.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.crews[0];
			require(approx(live.activityLevel, 0.5), "activity level not reported");
			require(approx(live.foodConsumed, 0.5 * 0.001 * 2.0 * elapsed),
				"half activity did not halve the food rate");
			require(approx(live.co2ProducedKg, 0.5 * 0.0011 * 2.0 * elapsed),
				"half activity did not halve the CO2 return");
			require(approx(live.retainedKg, 0.5 * (0.004 - 0.0033) * 2.0 * elapsed),
				"half activity did not halve the retained mass");
			require(live.status == core::StationTransferStatus::Full,
				"a half-activity crew with full stores is not fully supplied");
		}
		// Crew inputs and outputs never touch Agents: with crews running
		// against stores and a compartment, Agents behave exactly as in
		// the identical crew-free World.
		{
			core::World withCrew("CrewAgentsA", 8, 2);
			core::World withoutCrew("CrewAgentsB", 8, 2);
			withCrew.pauseSimulation();
			withoutCrew.pauseSimulation();
			auto const crewRoom = withCrew.addRoom("Bunk", 0, 0, 0, 7, 1);
			auto const controlRoom = withoutCrew.addRoom("Bunk", 0, 0, 0, 7, 1);
			withCrew.finishBuild();
			withoutCrew.finishBuild();
			withCrew.pauseSimulation();
			withoutCrew.pauseSimulation();
			require(crewConfigure(withCrew, fullStores(), compartment(), { crew(2, 1.0) }).first,
				"crew setup for the Agent isolation run refused");
			std::string diagnostic;
			core::StationSystemsSettings idle;
			idle.enabled = true;
			idle.observationIntervalSeconds = 1.0f;
			require(withoutCrew.configureStationSystems(idle, false, &diagnostic), diagnostic.c_str());
			withCrew.createAgent("Sleeper", crewRoom, 0u, 3.5f);
			withoutCrew.createAgent("Sleeper", controlRoom, 0u, 3.5f);
			withCrew.resumeSimulation();
			withoutCrew.resumeSimulation();
			require(smoke::station::advanceSeconds(withCrew, 2.0) && smoke::station::advanceSeconds(withoutCrew, 2.0),
				"Agent isolation run failed");
			auto const& a = withCrew.getSimulationSnapshot().agents;
			auto const& b = withoutCrew.getSimulationSnapshot().agents;
			require(a.size() == 1 && b.size() == 1,
				"the crew run did not preserve the authored Agent count");
			require(a[0].id == b[0].id && a[0].sectorId == b[0].sectorId
				&& a[0].localPosition.x == b[0].localPosition.x
				&& a[0].localPosition.y == b[0].localPosition.y
				&& a[0].state == b[0].state && a[0].pose == b[0].pose
				&& a[0].active == b[0].active && a[0].hasPath == b[0].hasPath,
				"crew metabolism mutated Agent state");
			require(withCrew.getStationSystemsSnapshot().crews[0].foodConsumed > 0.0,
				"the crew did not actually run alongside the Agent");
		}
		// Deterministic reset: the same authored scenario replays to the
		// same crew ledger and the same event history.
		{
			core::World world("CrewReset", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(crewConfigure(world, fullStores(), compartment(), { crew(2, 1.0) }).first,
				"reset-scenario crew refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 2.0), "reset scenario run failed");
			auto const saved = world.getStationSystemsSnapshot();
			auto const events = world.getStationSystemsEvents();
			world.pauseSimulation();
			world.resetSimulation();
			require(world.getStationSystemsSnapshot().crews[0].foodConsumed == 0.0
				&& world.getStationSystemsSnapshot().crews[0].retainedKg == 0.0,
				"reset did not reconstruct the crew ledger from the authored start");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 2.0), "reset replay failed");
			require(world.getStationSystemsSnapshot() == saved
				&& world.getStationSystemsEvents() == events,
				"crew scenario is not deterministic under reset");
		}
		// The Lua sandbox mirror: a corrupt crew descriptor crossing is
		// contained at construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"assert(type(crews) == 'table' and #crews == 1, 'crew descriptor crossing lost')\n"
					"assert(crews[1].foodRate > 0, 'corrupt crew rate crossing')\n"
					"error('corrupt crew descriptor crossing')\n"
					"end";
			});
			core::World corrupt("CrewCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const result = crewConfigure(corrupt, fullStores(), compartment(), { crew(2, 1.0) });
			require(!result.first && result.second.find("corrupt crew descriptor crossing")
				!= std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt crew sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
		// The sandbox's own crew closure audits run every tick: a model
		// that drifts from its ledger fails contained. The nominal run
		// above already exercised the mirror; here the sandbox asserts
		// the crew's uniform scale directly against the host-visible
		// compartment heat on the first tick.
		{
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled] {
				return std::string("local factory = (function()\n") + bundled
					+ "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"local crew, habitat = self.crews[1], self.atmospheres[1]\n"
					"assert(crew.lastScale >= 0 and crew.lastScale <= 1, 'sandbox crew scale out of range')\n"
					"assert(math.abs(crew.retainedKg - ((crew.foodConsumed + crew.waterConsumed + crew.oxygenConsumed)\n"
					"- (crew.co2Produced + crew.moistureProduced + crew.urineProduced + crew.organicWasteProduced))) < 1e-9,\n"
					"  'sandbox crew retained ledger does not close')\n"
					"assert(habitat.crewHeatKJ > 0.0, 'sandbox crew heat not booked into the compartment')\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandbox("SandboxCrew", 2, 2);
			sandbox.pauseSimulation(); sandbox.finishBuild();
			require(crewConfigure(sandbox, fullStores(), compartment(), { crew(2, 1.0) }).first,
				"sandbox crew scenario refused");
			sandbox.resumeSimulation();
			require(smoke::station::advanceSeconds(sandbox, 1.0),
				"bundled Lua crew mirror failed its in-sandbox assertions");
			core::setStationSystemsSourceLoader({});
		}
		// The Lua mirror refuses an aliased crew stream role at
		// construction (#600), independently of the host check: the host
		// rig here is valid, and the alias is introduced only inside the
		// Lua crossing - the bundled model's own constructor assert must
		// reject it, contained as a construction failure carrying the
		// refusal diagnostic, so Reset/replay reconstructs the same
		// contained state even if a crossing is tampered with.
		{
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled] {
				return std::string("local factory = (function()\n") + bundled
					+ "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"crews[1].waterStore = crews[1].foodStore\n"
					"return factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"end";
			});
			core::World sandbox("SandboxCrewAlias", 2, 2);
			sandbox.pauseSimulation(); sandbox.finishBuild();
			auto const result = crewConfigure(sandbox, fullStores(), compartment(), { crew(2, 1.0) });
			require(!result.first
				&& result.second.find("chemically distinct streams may not share one store")
					!= std::string::npos
				&& !sandbox.getStationSystemsSnapshot().enabled,
				("the Lua mirror did not refuse an aliased crew stream crossing: "
					+ result.second).c_str());
			core::setStationSystemsSourceLoader({});
		}
	}

	// Water recovery and qualification (#402): collectors condense compartment
	// vapour onto a coolant-loop cold surface into a raw water store with the
	// latent heat routed to the loop exactly once, and treatments split raw
	// water into treated water plus a retained concentrate for declared
	// electricity and consumables, qualifying the output only on a full
	// rated power dose.
	void water(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto plainStore = [](std::string const& id, double capacity, double starting)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			return settings;
		};
		// 100 m3 at 25 C holds about 2.30 kg of vapour at saturation, so a
		// 2.0 kg starting inventory sits near 87% relative humidity and a 0.5
		// target leaves roughly 0.85 kg condensable.
		auto compartment = []()
		{
			core::AtmosphereSettings settings;
			settings.id = "habitat";
			settings.volumeM3 = 100.0;
			settings.startingOxygenKg = 20.0;
			settings.startingNitrogenKg = 80.0;
			settings.startingWaterVapourKg = 2.0;
			settings.startingTemperatureC = 25.0;
			settings.thermalCapacityKJPerK = 10000.0;
			settings.minPressureKPa = 1.0;
			settings.maxPressureKPa = 200.0;
			return settings;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = 10000.0;
			settings.startingTemperatureC = 10.0;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto coldLoop = [](std::string const& id, double startingCoolantKg, double startingC)
		{
			core::CoolantLoopSettings settings;
			settings.id = id;
			settings.reservoirId = "sink";
			settings.coolantCapacityKg = 100.0;
			settings.startingCoolantKg = startingCoolantKg;
			settings.specificHeatKJPerKgK = 4.0;
			settings.startingTemperatureC = startingC;
			settings.freezeProtectionSetpointC = 0.0;
			settings.pumpPowerKW = 0.1;
			settings.ratedFlowKgPerS = 10.0;
			settings.exchangerConductanceKWPerK = 0.0;
			settings.heaterCapacityKW = 0.0;
			return settings;
		};
		auto collector = [](std::string const& id, double rate, double targetRh)
		{
			core::WaterCollectorSettings settings;
			settings.id = id;
			settings.atmosphereId = "habitat";
			settings.coolingLoopId = "chilled";
			settings.thermalClass = "moderate";
			settings.destStoreId = "raw";
			settings.maxRateKgPerS = rate;
			settings.targetRelativeHumidity = targetRh;
			return settings;
		};
		// 0.005 kg/s throughput at 80% recovery: 0.004 kg/s treated and
		// 0.001 kg/s concentrate, costing 9 kW (0.005 kg/s x 0.5 kWh/kg
		// x 3600 s/h) and 5e-5 consumable units per second.
		auto treatment = [](std::string const& id)
		{
			core::WaterTreatmentSettings settings;
			settings.id = id;
			settings.sourceStoreId = "raw";
			settings.treatedStoreId = "treated";
			settings.concentrateStoreId = "concentrate";
			settings.consumableStoreId = "consumable";
			settings.throughputKgPerS = 0.005;
			settings.recoveryRatio = 0.8;
			settings.specificEnergyKWhPerKg = 0.5;
			settings.consumablePerKg = 0.01;
			return settings;
		};
		auto waterStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("raw", 100.0, 50.0));
			stores.push_back(plainStore("treated", 100.0, 0.0));
			stores.push_back(plainStore("concentrate", 100.0, 0.0));
			stores.push_back(plainStore("consumable", 100.0, 10.0));
			return stores;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto waterConfigure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::CoolantLoopSettings> loops,
			std::vector<core::WaterCollectorSettings> collectors,
			std::vector<core::WaterTreatmentSettings> treatments,
			double solarKW)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { compartment() };
			settings.thermalReservoirs = { reservoir("sink", "moderate") };
			settings.coolantLoops = std::move(loops);
			settings.waterCollectors = std::move(collectors);
			settings.waterTreatments = std::move(treatments);
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
		// Refused configurations: every malformed water contract is rejected
		// before anything runs, and none of the refusals mutates the World.
		{
			core::World bad("WaterBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = waterStores();
			settings.atmospheres = { compartment() };
			settings.thermalReservoirs = { reservoir("sink", "moderate") };
			settings.coolantLoops = { coldLoop("chilled", 100.0, 5.0) };
			std::string diagnostic;
			settings.waterCollectors = { collector("cond", 0.01, 0.5) };
			settings.waterCollectors[0].atmosphereId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown atmosphere compartment") != std::string::npos,
				"a collector serving an unknown compartment was accepted");
			settings.waterCollectors = { collector("cond", 0.01, 0.5) };
			settings.waterCollectors[0].coolingLoopId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown coolant loop") != std::string::npos,
				"a collector cold surface on an unknown loop was accepted");
			// #752: a hazardous-class collector on a normal compartment is
			// accepted - condensate is just water, whatever air it
			// condensed from, and a collector may serve any compartment.
			settings.waterCollectors = { collector("cond", 0.01, 0.5) };
			require(core::validateStationSystemsSettings(settings, diagnostic),
				("a collector serving across the class boundary was refused: " + diagnostic).c_str());
			settings.waterCollectors = { collector("cond", 0.01, 1.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("target relative humidity") != std::string::npos,
				"a humidity target above one was accepted");
			settings.waterCollectors = { collector("cond", -1.0, 0.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("maximum collection rate") != std::string::npos,
				"a negative collection rate was accepted");
			settings.waterCollectors = { collector("cond", 0.01, 0.5) };
			settings.waterCollectors[0].thermalClass = "warm";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("does not match coolant loop") != std::string::npos,
				"a collector whose service class mismatches its loop was accepted");
			// A compound destination would turn pure condensate into dry mass.
			settings.waterCollectors = { collector("cond", 0.01, 0.5) };
			settings.stores = waterStores();
			core::ResourceCompositionSettings muddy;
			muddy.moisture = 0.5;
			settings.stores[0].composition = muddy;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("plain mass stores") != std::string::npos,
				"a collector delivering into a compound store was accepted");
			// A store shared by a normal collector and a hazardous
			// treatment is accepted (#751): the class no longer reserves
			// a store for one process, and the shared inventory is the
			// consequence.
			settings.waterCollectors = { collector("cond", 0.01, 0.5) };
			settings.stores = waterStores();
			settings.waterTreatments = { treatment("plant") };
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"a raw water store shared by normal and hazardous processes was refused");
			settings.waterTreatments = { treatment("plant") };
			settings.waterTreatments[0].recoveryRatio = 0.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("recovery ratio") != std::string::npos,
				"a recovery ratio of zero was accepted");
			settings.waterTreatments = { treatment("plant") };
			settings.waterTreatments[0].treatedStoreId = "raw";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("distinct source, treated and concentrate")
					!= std::string::npos,
				"a treatment drawing and delivering to one store was accepted");
			settings.waterTreatments = { treatment("plant") };
			settings.waterTreatments[0].specificEnergyKWhPerKg = -1.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("specific energy") != std::string::npos,
				"a negative specific energy was accepted");
			settings.waterTreatments = {};
			std::vector<core::WaterCollectorSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationWaterCollectors; ++index)
				tooMany.push_back(collector("c" + std::to_string(index), 0.01, 0.5));
			settings.waterCollectors = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared collector budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused water recovery configuration mutated the World");
		}
		// Nominal recovery: the collector runs at its configured rate, the
		// latent heat lands in the coolant loop and nowhere else, and the
		// treatment splits the raw water and qualifies its output on full
		// power while the concentrate stays retained unqualified.
		{
			core::World world("WaterNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", 0.01, 0.5) },
				{ treatment("plant") }, 10.0).first, "nominal water recovery refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "nominal water recovery run failed");
			auto const elapsed = 60.0 * dt;
			auto const expectedKg = 0.01 * elapsed;
			auto const latentKJ = expectedKg * 2257.0;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.waterCollectors.size() == 1 && snap.waterTreatments.size() == 1,
				"water recovery contracts missing from the snapshot");
			auto const& live = snap.waterCollectors[0];
			require(live.status == core::StationTransferStatus::Full
				&& approx(live.collectedKg, expectedKg),
				"the collector did not run at its configured rate");
			require(live.coldSurfaceAvailable && live.blockReason.empty(),
				"a working collector reports a blocked cold surface");
			require(approx(live.latentHeatEnergyKJ, latentKJ),
				"the latent heat routed to the coolant loop does not match the condensate");
			// The compartment booked the removal and NOT the latent heat: the
			// condensation's heat went into the coolant, once.
			auto const& habitat = snap.atmospheres[0];
			require(approx(habitat.collectedKg, expectedKg),
				"the compartment's collected-water ledger is wrong");
			require(habitat.latentHeatEnergyKJ == 0.0,
				"the compartment booked the collector's latent heat a second time");
			require(approx(habitat.waterVapourKg, 2.0 - expectedKg),
				"the compartment vapour did not fall by exactly what was collected");
			// The loop carries the collector heat inside its own closing ledger.
			auto const& loop = snap.coolantLoops[0];
			require(approx(loop.collectorHeatEnergyKJ, latentKJ),
				"the coolant loop ledger is missing the water-recovery heat");
			require(std::abs(loop.residualKJ) < 1e-6, "coolant loop ledger not closed");
			require(loop.loopTemperatureC > 5.0, "the collector heat did not warm the loop");
			// The treatment ran at full rated power and qualified its output.
			auto const& plant = snap.waterTreatments[0];
			auto const processed = 0.005 * elapsed;
			require(plant.status == core::StationTransferStatus::Full
				&& approx(plant.scale, 1.0), "the treatment is not running at full rate");
			require(approx(plant.processedKg, processed), "processed mass mismatch");
			require(approx(plant.treatedKg, processed * 0.8), "treated mass mismatch");
			require(approx(plant.concentrateKg, processed * 0.2), "concentrate mass mismatch");
			require(plant.qualityAccepted && approx(plant.acceptedKg, plant.treatedKg)
				&& approx(plant.heldKg, 0.0), "a full-power treatment output was not accepted");
			require(approx(plant.energySuppliedKJ, plant.energyOwedKJ),
				"a fully powered treatment did not receive its full energy dose");
			// The owed ledger is the physical dose: processed kg x 0.5 kWh/kg
			// x 3600 kJ/kWh (#566).
			require(approx(plant.energyOwedKJ, processed * 0.5 * 3600.0),
				"the energy-owed ledger does not match the rated kWh dose");
			require(approx(plant.consumablesUsed, processed * 0.01), "consumable usage mismatch");
			require(std::abs(plant.residualKg) < 1e-6,
				"the processed/treated/concentrate ledger does not close");
			// Store placement: treated qualified, concentrate retained
			// unqualified and therefore unusable pending disposal.
			auto const treatedStore = findStore(snap, "treated");
			auto const concentrateStore = findStore(snap, "concentrate");
			auto const rawStore = findStore(snap, "raw");
			require(approx(treatedStore.currentQualified, processed * 0.8)
				&& treatedStore.currentUnqualified == 0.0,
				"accepted treated water is not on the qualified side");
			require(approx(concentrateStore.currentUnqualified, processed * 0.2)
				&& concentrateStore.currentQualified == 0.0,
				"retained concentrate is not held unqualified");
			require(approx(rawStore.currentQualified, 50.0 - processed + expectedKg),
				"the raw water store gained or lost the wrong amount");
			// The per-unit boundary carries the collected inflow and closes.
			require(snap.balances.size() == 1, "per-unit boundary ledger incomplete");
			require(approx(snap.balances[0].collected, expectedKg),
				"the boundary ledger is missing the collected-water inflow");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// A classless collector has the same physical result against the
		// classless coolant loop (#757, #758). Configuration also
		// constructs the bundled Lua mirror, so its constructor must
		// accept the combination.
		{
			core::World world("WaterCrossClass", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const chilled = coldLoop("chilled", 100.0, 5.0);
			auto const [ok, diagnostic] = waterConfigure(world, waterStores(), { chilled },
				{ collector("cond", 0.01, 0.5) },
				{ treatment("plant") }, 10.0);
			require(ok, ("a cross-class collector configuration was refused: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "cross-class collection run failed");
			auto const elapsed = 60.0 * dt;
			auto const expectedKg = 0.01 * elapsed;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.waterCollectors[0].status == core::StationTransferStatus::Full
				&& approx(snap.waterCollectors[0].collectedKg, expectedKg),
				"the cross-class collector did not run at its configured rate");
			require(approx(findStore(snap, "raw").currentQualified,
				50.0 - 0.005 * elapsed + expectedKg),
				"the cross-class condensate did not land in the destination store");
			auto const latentKJ = expectedKg * 2257.0;
			require(approx(snap.waterCollectors[0].latentHeatEnergyKJ, latentKJ)
				&& approx(snap.coolantLoops[0].collectorHeatEnergyKJ, latentKJ),
				"cross-class collection did not route its latent heat exactly once");
			require(approx(snap.atmospheres[0].waterVapourKg, 2.0 - expectedKg)
				&& approx(snap.atmospheres[0].collectedKg, expectedKg)
				&& snap.atmospheres[0].latentHeatEnergyKJ == 0.0,
				"cross-class collection did not close the compartment water and heat ledgers");
			require(approx(snap.coolantLoops[0].coolantKg, 100.0),
				"cross-class collection moved coolant through the heat-only interface");
			require(std::abs(snap.coolantLoops[0].residualKJ) < 1e-6
				&& std::abs(snap.thermal.reservoirs[0].residualKJ) < 1e-6
				&& std::abs(snap.waterTreatments[0].residualKg) < 1e-6,
				"a water or thermal ledger did not close on cross-class condensate");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Curtailed power: the process runs on a partial energy dose, so the
		// water it made is held unqualified rather than passed through.
		{
			core::World world("WaterCurtailed", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			// The array covers the loop pump (0.1 kW) and only 1.0 kW of the
			// treatment's 9 kW demand.
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", 0.01, 0.5) },
				{ treatment("plant") }, 1.1).first, "curtailed water setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "curtailed water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& plant = snap.waterTreatments[0];
			require(plant.status == core::StationTransferStatus::Partial,
				"a curtailed treatment is not reported as partial");
			require(!plant.qualityAccepted && plant.heldKg > 0.0
				&& approx(plant.acceptedKg, 0.0),
				"a curtailed treatment output was accepted as qualified");
			require(approx(plant.heldKg, plant.treatedKg),
				"not all of the curtailed output was held");
			require(!plant.holdReason.empty(), "held output carries no hold reason");
			require(plant.energySuppliedKJ < plant.energyOwedKJ,
				"a curtailed treatment received its full energy dose anyway");
			require(plant.lastPowerSuppliedKW > 0.0
				&& plant.lastPowerSuppliedKW < plant.lastPowerDemandKW,
				"the curtailed power draw is not visible");
			auto const treatedStore = findStore(snap, "treated");
			require(treatedStore.currentQualified == 0.0
				&& treatedStore.currentUnqualified > 0.0,
				"held treated water is not on the unqualified side");
			// The collector is unaffected: it needs no electricity of its own.
			require(snap.waterCollectors[0].collectedKg > 0.0,
				"a power-curtailed treatment stopped water collection");
		}
		// No power at all: the treatment is refused whole and consumes
		// neither water nor consumables - it does not run for free.
		{
			core::World world("WaterNoPower", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			// The array exactly covers the loop pump: nothing is left over.
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", 0.01, 0.5) },
				{ treatment("plant") }, 0.1).first, "zero-power water setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "zero-power water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& plant = snap.waterTreatments[0];
			require(plant.status == core::StationTransferStatus::Rejected,
				"an unfed treatment is not refused");
			require(plant.processedKg == 0.0 && plant.treatedKg == 0.0
				&& plant.consumablesUsed == 0.0,
				"an unfed treatment moved water or consumed consumables");
			require(plant.holdReason.find("no electrical supply") != std::string::npos,
				"the whole-tick refusal carries no reason");
			require(approx(findStore(snap, "consumable").currentQualified, 10.0),
				"an unfed treatment drew consumables");
		}
		// Cooling unavailable: with no coolant circulating across the cold
		// surface nothing condenses, and the reason stays visible rather than
		// the collector quietly idling.
		{
			core::World world("WaterNoCooling", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 0.0, 5.0) },
				{ collector("cond", 0.01, 0.5) },
				{ treatment("plant") }, 1.0).first, "no-cooling water setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "no-cooling water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.waterCollectors[0];
			require(live.status == core::StationTransferStatus::Rejected
				&& live.collectedKg == 0.0 && !live.coldSurfaceAvailable,
				"a collector with a dry loop collected condensate anyway");
			require(live.blockReason.find("no coolant circulation") != std::string::npos,
				"the dry-loop collection block carries no reason");
			require(snap.atmospheres[0].collectedKg == 0.0,
				"the compartment booked a collection that never happened");
			// The treatment still runs off the raw water already stored.
			require(snap.waterTreatments[0].processedKg > 0.0,
				"a blocked collector stopped the treatment from running");
		}
		// A cold surface that is not colder than the compartment air cannot
		// condense: the gate is the temperature difference, not the loop's
		// mere existence.
		{
			core::World world("WaterWarmSurface", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 100.0, 30.0) },
				{ collector("cond", 0.01, 0.5) },
				{ treatment("plant") }, 1.0).first, "warm-surface water setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "warm-surface water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.waterCollectors[0].collectedKg == 0.0
				&& snap.waterCollectors[0].blockReason.find("not colder") != std::string::npos,
				"a cold surface warmer than the air collected condensate");
		}
		// The humidity target is a real stop: once the compartment holds no
		// vapour above it, collection stops rather than stripping the air.
		{
			core::World world("WaterDryAir", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", 0.01, 0.8) },
				{ treatment("plant") }, 1.0).first, "dry-air water setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 20.0), "dry-air water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.waterCollectors[0];
			// 100 m3 at 25 C saturates near 2.298 kg of vapour, so a 2.0 kg
			// inventory against a 0.8 target leaves about 0.162 kg condensable:
			// the collector runs briefly and then stops on the target rather
			// than stripping the air dry.
			require(live.collectedKg > 0.15 && live.collectedKg < 0.17,
				"the collector did not stop at the humidity target");
			require(live.status == core::StationTransferStatus::Rejected
				&& live.blockReason.find("no vapour above the collection target")
					!= std::string::npos,
				"the humidity-target stop is not reported");
		}
		// Destination capacity binds: a full treated store caps the process
		// and the limiter names it, and the concentrate store does the same
		// for the retained stream.
		{
			core::World world("WaterFullTreated", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = waterStores();
			stores[1].capacity = 0.1;
			require(waterConfigure(world, stores, { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", 0.01, 0.5) },
				{ treatment("plant") }, 1.0).first, "full-treated water setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 30.0), "full-treated water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const treatedStore = findStore(snap, "treated");
			require(treatedStore.currentQualified <= 0.1 + 1e-9,
				"the treated store was pushed past its capacity");
			require(snap.waterTreatments[0].limiter.find("treated water store")
					!= std::string::npos,
				"the treated-store cap is not reported as the limiter");
			require(snap.waterTreatments[0].processedKg < 0.005 * 120.0 * dt * 60.0,
				"the treatment ran at full rate against a full destination");
		}
		// Consumable exhaustion caps the process the same way: the machine
		// cannot run on media it does not have.
		{
			core::World world("WaterNoConsumable", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = waterStores();
			stores[3].startingQualified = 0.0001;
			require(waterConfigure(world, stores, { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", 0.01, 0.5) },
				{ treatment("plant") }, 1.0).first,
				"scarce-consumable water setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 5.0), "scarce-consumable water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(approx(snap.waterTreatments[0].consumablesUsed, 0.0001),
				"the treatment used consumables it did not have");
			require(snap.waterTreatments[0].limiter.find("consumable") != std::string::npos,
				"the consumable cap is not reported as the limiter");
			require(findStore(snap, "consumable").currentQualified <= 1e-12,
				"the consumable store still holds what the process needed");
		}
		// The ledger closes across a long run and the whole thing replays
		// identically after a reset: recovery is deterministic, not a
		// function of how the ticks were batched.
		{
			core::World world("WaterReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", 0.01, 0.5) },
				{ treatment("plant") }, 1.0).first, "replay water setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.5), "replay water run failed");
			auto const first = world.getStationSystemsSnapshot();
			world.pauseSimulation(); world.resetSimulation(); world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.5), "replay water re-run failed");
			auto const second = world.getStationSystemsSnapshot();
			require(approx(second.waterCollectors[0].collectedKg,
				first.waterCollectors[0].collectedKg),
				"water collection is not deterministic across a reset");
			require(approx(second.waterTreatments[0].processedKg,
				first.waterTreatments[0].processedKg),
				"water treatment is not deterministic across a reset");
			require(approx(second.coolantLoops[0].collectorHeatEnergyKJ,
				first.coolantLoops[0].collectorHeatEnergyKJ),
				"the loop's water-recovery ledger is not deterministic across a reset");
		}
		// The Lua sandbox mirror: a corrupt water descriptor crossing is
		// contained at construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"assert(type(waterCollectors) == 'table' and #waterCollectors == 1, 'water collector descriptor crossing lost')\n"
					"assert(waterCollectors[1].maxRateKgPerS > 0, 'corrupt collector rate crossing')\n"
					"error('corrupt water collector descriptor crossing')\n"
					"end";
			});
			core::World corrupt("WaterCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const result = waterConfigure(corrupt, waterStores(),
				{ coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", 0.01, 0.5) },
				{ treatment("plant") }, 1.0);
			require(!result.first && result.second.find("corrupt water collector descriptor crossing")
				!= std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt water collector sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
		// A treatment whose sandbox mirror finds a broken recovery split is
		// contained the same way.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"assert(type(waterTreatments) == 'table' and #waterTreatments == 1, 'water treatment descriptor crossing lost')\n"
					"assert(waterTreatments[1].recoveryRatio > 0 and waterTreatments[1].recoveryRatio <= 1, 'corrupt recovery ratio crossing')\n"
					"error('corrupt water treatment descriptor crossing')\n"
					"end";
			});
			core::World corrupt("WaterTreatmentCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const result = waterConfigure(corrupt, waterStores(),
				{ coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", 0.01, 0.5) },
				{ treatment("plant") }, 1.0);
			require(!result.first && result.second.find("corrupt water treatment descriptor crossing")
				!= std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt water treatment sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}

	// #406: fuel-cell backup from dedicated reactant stores. The cell is a
	// dispatchable source whose every axis is finite: the installed rating,
	// the reactants drawable above their protected reserves, the product
	// water destination's headroom and a qualified class-matched cooling
	// sink. The reaction closes on mass (hydrogen + oxygen = water) and on
	// energy (chemical = electrical + waste), the endurance clock counts
	// only usable reactants, and protected oxygen is never drawn
	// implicitly - the cell can only ever see its own dedicated store.
	void fuelCells(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto const o2PerH2 = core::StationFuelCellOxygenMassPerHydrogenKg;
		auto const lhv = core::StationElectrolysisChemicalEnergyHydrogenKJPerKg;
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
		// 10 kW installed, 60% of the hydrogen's chemical energy to
		// electricity, waste heat into the class-matched sink.
		auto fuelCell = [](std::string const& id)
		{
			core::FuelCellSettings settings;
			settings.id = id;
			settings.hydrogenStoreId = "h2";
			settings.oxygenStoreId = "o2";
			settings.waterStoreId = "productWater";
			settings.thermalClass = "low";
			settings.heatSinkId = "sink";
			settings.ratedOutputKW = 10.0;
			settings.efficiency = 0.6;
			settings.commandedOutputKW = 6.0;
			settings.maxSinkTemperatureC = 80.0;
			return settings;
		};
		auto baseStores = [&plainStore]
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("h2", 10.0, 10.0));
			stores.push_back(plainStore("o2", 100.0, 100.0));
			stores.push_back(plainStore("productWater", 200.0, 0.0));
			return stores;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto configure = [&](core::World& world,
			core::StationSystemsSettings settings)
		{
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			std::string diagnostic;
			return world.configureStationSystems(settings, false, &diagnostic);
		};
		// Refused configurations: every malformed backup contract is
		// rejected before anything runs, and none of the refusals mutates
		// the World.
		{
			core::World bad("FuelCellBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = baseStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			std::string diagnostic;
			settings.fuelCells = { fuelCell("cell") };
			settings.fuelCells[0].hydrogenStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("hydrogen reactant store 'ghost' does not exist") != std::string::npos,
				"a fuel cell fed by an unknown hydrogen store was accepted");
			settings.fuelCells = { fuelCell("cell") };
			settings.fuelCells[0].waterStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("product water store 'ghost' does not exist") != std::string::npos,
				"a fuel cell with an unknown product water store was accepted");
			settings.fuelCells = { fuelCell("cell") };
			settings.fuelCells[0].efficiency = 0.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("efficiency must be greater than 0") != std::string::npos,
				"a zero-efficiency fuel cell was accepted");
			settings.fuelCells = { fuelCell("cell") };
			settings.fuelCells[0].efficiency = 1.5;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("efficiency must be greater than 0") != std::string::npos,
				"an over-unity fuel cell was accepted");
			settings.fuelCells = { fuelCell("cell") };
			settings.fuelCells[0].commandedOutputKW = 12.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("within the installed rating") != std::string::npos,
				"a commanded output above the installed rating was accepted");
			settings.fuelCells = { fuelCell("cell") };
			settings.fuelCells[0].maxSinkTemperatureC = -300.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"a maximum sink temperature below absolute zero was accepted");
			settings.fuelCells = { fuelCell("cell") };
			settings.fuelCells[0].thermalClass = "moderate";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("does not match heat sink") != std::string::npos,
				"a thermal class mismatch against the heat sink was accepted");
			// A gas store shared between a normal and a hazardous process
			// is accepted (#751): both cells wire to it and the shared
			// inventory is the consequence.
			settings.fuelCells = { fuelCell("cell"), fuelCell("hcell") };
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"a shared gas store across hazard classes was refused");
			std::vector<core::FuelCellSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationFuelCells; ++index)
				tooMany.push_back(fuelCell("cell" + std::to_string(index)));
			settings = core::StationSystemsSettings{};
			settings.enabled = true;
			settings.stores = baseStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared fuel-cell budget was accepted");
			// Aliased reactant stores (#585), the analogous gas-process
			// pattern: the cell draws each reactant from its own store's
			// drawable stock, so one store named for both spends that stock
			// twice against a stoichiometry that expects two.
			settings = core::StationSystemsSettings{};
			settings.enabled = true;
			settings.stores = baseStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell") };
			settings.fuelCells[0].oxygenStoreId = "h2";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("both the hydrogen and the oxygen reactant") != std::string::npos,
				"a fuel cell aliasing its hydrogen and oxygen reactant stores was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused fuel-cell configuration mutated the World");
		}
		// Nominal backup: the cell delivers its commanded output into the
		// shared pool, the load runs on it, the reaction closes on mass and
		// energy, the product water lands in its store, the sink carries
		// the waste heat, and the endurance clock is exposed.
		{
			core::World world("FuelCellNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.stores = baseStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell") };
			core::EquipmentLoadSettings load;
			load.id = "shop";
			load.electricalDemandKW = 5.0;
			load.thermalClass = "low";
			load.coolingDisposition = core::StationCoolingDisposition::Exported;
			settings.equipmentLoads = { load };
			require(configure(world, settings), "nominal fuel-cell setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "nominal fuel-cell run failed");
			auto const elapsed = 600.0 * dt;
			auto const h2Kg = 6.0 * elapsed / (0.6 * lhv);
			auto const o2Kg = h2Kg * o2PerH2;
			auto const waterKg = h2Kg + o2Kg;
			auto const chemicalKJ = h2Kg * lhv;
			auto const electricalKJ = 6.0 * elapsed;
			auto const wasteKJ = chemicalKJ - electricalKJ;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.fuelCells.size() == 1, "fuel cell missing from the snapshot");
			auto const& cell = snap.fuelCells[0];
			require(cell.status == core::StationTransferStatus::Full
				&& cell.coolingAvailable && approx(cell.lastOutputKW, 6.0),
				"the fuel cell is not delivering its commanded output with a qualified sink");
			require(approx(cell.hydrogenConsumedKg, h2Kg)
				&& approx(cell.oxygenConsumedKg, o2Kg)
				&& approx(cell.waterProducedKg, waterKg),
				"the reaction did not follow the declared stoichiometry");
			require(std::abs(cell.residualKg) < 1e-6,
				"the hydrogen/oxygen/water mass ledger does not close");
			require(approx(cell.chemicalEnergyKJ, chemicalKJ)
				&& approx(cell.electricalKJ, electricalKJ)
				&& approx(cell.wasteHeatKJ, wasteKJ)
				&& std::abs(cell.residualKJ) < 1e-6,
				"the chemical/electrical/waste energy ledger does not close");
			require(approx(findStore(snap, "h2").currentQualified, 10.0 - h2Kg)
				&& approx(findStore(snap, "o2").currentQualified, 100.0 - o2Kg)
				&& approx(findStore(snap, "productWater").currentQualified, waterKg),
				"the reactant and product stores did not follow the reaction");
			require(approx(snap.power.fuelCellOutputKW, 6.0),
				"the power snapshot does not report the fuel-cell output");
			require(approx(snap.thermal.reservoirs[0].fuelCellEnergyKJ, wasteKJ),
				"the waste heat did not land in the sink reservoir");
			// The load runs on the backup: no solar, no batteries, the
			// cell's output is the only supply in the pool.
			bool loadSupplied = false;
			for (auto const& entry : snap.power.loads)
				if (entry.id == "shop" && approx(entry.suppliedKW, 5.0)) loadSupplied = true;
			require(loadSupplied, "the equipment load did not run on the fuel-cell backup");
			// Endurance: the remaining hydrogen at the actual output rate.
			auto const remainingH2 = 10.0 - h2Kg;
			auto const expectedEndurance = remainingH2 * 0.6 * lhv / 6.0;
			require(approx(cell.enduranceSeconds, expectedEndurance),
				"the backup endurance clock does not match the usable reactants");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Reactant exhaustion: the cell runs until the hydrogen store is
		// spent, then stops whole with a stated reason; the endurance clock
		// reaches zero and no reactant vanishes.
		{
			core::World world("FuelCellDry", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = baseStores();
			// Enough hydrogen for about six seconds at 6 kW.
			for (auto& store : stores)
				if (store.id == "h2") store.startingQualified = 5e-4;
			core::StationSystemsSettings settings;
			settings.stores = stores;
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell") };
			require(configure(world, settings), "dry hydrogen setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "dry hydrogen run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.fuelCells[0];
			require(approx(cell.hydrogenConsumedKg, 5e-4)
				&& findStore(snap, "h2").currentQualified < 1e-9,
				"the cell did not consume exactly the hydrogen it had");
			require(cell.status == core::StationTransferStatus::Rejected
				&& (cell.limiter.find("hydrogen store") != std::string::npos
					|| cell.blockReason.find("hydrogen store") != std::string::npos),
				"the exhausted-hydrogen refusal is not stated");
			require(cell.enduranceSeconds < 1e-6,
				"the endurance clock did not reach zero with the hydrogen spent");
			require(approx(cell.waterProducedKg, 5e-4 * (1.0 + o2PerH2)),
				"the product water does not match the consumed hydrogen");
			require(cell.electricalKJ <= 5e-4 * 0.6 * lhv + 1e-6,
				"the cell delivered more electrical energy than its hydrogen could pay for");
		}
		// Reserve-transfer refusal: the fuel-cell oxygen store is refilled
		// only through a controlled surplus transfer that preserves the
		// supplying ECLSS store's protected breathing reserve. With the
		// ECLSS store at its reserve the transfer is refused whole, the
		// dedicated store stays empty, and the cell is refused for want of
		// oxygen - the protected reserve is never touched.
		{
			core::World world("FuelCellReserve", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("h2", 10.0, 10.0));
			stores.push_back(plainStore("eclssO2", 100.0, 15.0, 15.0));
			stores.push_back(plainStore("fcO2", 100.0, 0.0));
			stores.push_back(plainStore("productWater", 200.0, 0.0));
			core::StationSystemsSettings settings;
			settings.stores = stores;
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell") };
			settings.fuelCells[0].oxygenStoreId = "fcO2";
			core::ResourceTransferSettings surplus;
			surplus.id = "eclssSurplus";
			surplus.sourceId = "eclssO2";
			surplus.destinationId = "fcO2";
			surplus.unit = "kg";
			surplus.ratePerSecond = 0.01;
			settings.transfers = { surplus };
			require(configure(world, settings), "reserve-transfer refusal setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "reserve-transfer refusal run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(approx(findStore(snap, "eclssO2").currentQualified, 15.0),
				"the protected breathing oxygen reserve was drawn down");
			require(approx(findStore(snap, "fcO2").currentQualified, 0.0),
				"oxygen reached the fuel-cell store from somewhere other than the surplus");
			auto const& cell = snap.fuelCells[0];
			require(cell.status == core::StationTransferStatus::Rejected
				&& (cell.limiter.find("oxygen store") != std::string::npos
					|| cell.blockReason.find("oxygen store") != std::string::npos),
				"the oxygen-starved fuel-cell refusal is not stated");
			require(cell.hydrogenConsumedKg < 1e-12 && cell.electricalKJ < 1e-12,
				"the cell burned hydrogen with no oxygen available");
			bool transferRefused = false;
			for (auto const& entry : snap.transfers)
				if (entry.id == "eclssSurplus"
					&& entry.status == core::StationTransferStatus::Rejected)
					transferRefused = true;
			require(transferRefused, "the surplus transfer into a protected source was not refused");
		}
		// Controlled surplus that is actually available: the transfer
		// fills the dedicated store above the ECLSS reserve, and the cell
		// runs on transferred oxygen while the protected reserve stays
		// untouched.
		{
			core::World world("FuelCellSurplus", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("h2", 10.0, 10.0));
			stores.push_back(plainStore("eclssO2", 100.0, 20.0, 15.0));
			stores.push_back(plainStore("fcO2", 100.0, 0.0));
			stores.push_back(plainStore("productWater", 200.0, 0.0));
			core::StationSystemsSettings settings;
			settings.stores = stores;
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell") };
			settings.fuelCells[0].oxygenStoreId = "fcO2";
			core::ResourceTransferSettings surplus;
			surplus.id = "eclssSurplus";
			surplus.sourceId = "eclssO2";
			surplus.destinationId = "fcO2";
			surplus.unit = "kg";
			surplus.ratePerSecond = 1.0;
			settings.transfers = { surplus };
			require(configure(world, settings), "surplus-fill setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "surplus-fill run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(findStore(snap, "eclssO2").currentQualified >= 15.0 - 1e-9,
				"the surplus transfer ate into the protected breathing reserve");
			require(findStore(snap, "fcO2").currentQualified >= 4.9,
				"the surplus transfer did not fill the dedicated oxygen store");
			auto const& cell = snap.fuelCells[0];
			require(cell.status == core::StationTransferStatus::Full
				&& approx(cell.lastOutputKW, 6.0),
				"the cell did not run on the transferred surplus oxygen");
		}
		// Blocked water destination: a full product water store caps the
		// reaction - the cell is refused rather than vanishing product
		// mass, and the consumed reactants still equal the delivered water.
		{
			core::World world("FuelCellWaterBlocked", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = baseStores();
			for (auto& store : stores)
				if (store.id == "productWater")
				{
					store.capacity = 0.005;
					store.startingQualified = 0.004;
				}
			core::StationSystemsSettings settings;
			settings.stores = stores;
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell") };
			require(configure(world, settings), "blocked-water setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "blocked-water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.fuelCells[0];
			require(findStore(snap, "productWater").currentQualified <= 0.005 + 1e-9,
				"product water overflowed its destination store");
			require(cell.status == core::StationTransferStatus::Rejected
				&& (cell.limiter.find("product water store") != std::string::npos
					|| cell.blockReason.find("product water store") != std::string::npos),
				"the blocked-water-destination refusal is not stated");
			require(approx(cell.waterProducedKg, 0.001)
				&& approx(cell.hydrogenConsumedKg + cell.oxygenConsumedKg,
					cell.waterProducedKg),
				"mass vanished or appeared at the blocked water destination");
		}
		// Cooling refusal: a sink at or above the maximum sink temperature
		// refuses the cell whole - no output, no reactant draw, and the
		// whole commanded output stays visible as unmet service.
		{
			core::World world("FuelCellHotSink", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.stores = baseStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 90.0) };
			settings.fuelCells = { fuelCell("cell") };
			require(configure(world, settings), "hot-sink fuel-cell setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "hot-sink fuel-cell run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.fuelCells[0];
			require(!cell.coolingAvailable
				&& cell.status == core::StationTransferStatus::Rejected,
				"a hot sink did not refuse the fuel cell");
			require(cell.hydrogenConsumedKg < 1e-12 && cell.electricalKJ < 1e-12,
				"the cell burned reactants while cooling was refused");
			require(approx(cell.lastUnmetOutputKW, 6.0),
				"the refused command is not visible as unmet output");
		}
		// The cell never draws below its own store's protected reserve:
		// a dedicated reactant store whose whole inventory is protected is
		// as good as empty for the backup.
		{
			core::World world("FuelCellProtectedReactant", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = baseStores();
			for (auto& store : stores)
				if (store.id == "h2") store.protectedReserve = 10.0;
			core::StationSystemsSettings settings;
			settings.stores = stores;
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell") };
			require(configure(world, settings), "protected-reactant setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "protected-reactant run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.fuelCells[0];
			require(cell.status == core::StationTransferStatus::Rejected
				&& cell.hydrogenConsumedKg < 1e-12,
				"the cell drew protected reactant inventory");
			require(approx(findStore(snap, "h2").currentQualified, 10.0),
				"the protected reactant store changed at all");
		}
		// Electrolysis/fuel-cell cycle: the cell burns the hydrogen the
		// electrolyzer made and returns the water and oxygen it split, so
		// the mass closes around the loop while the energy is lost at both
		// ends - the round trip recovers only the cell's efficiency times
		// the chemical energy the dose paid for.
		{
			core::World world("FuelCellCycle", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("water", 100.0, 10.0));
			stores.push_back(plainStore("o2", 100.0, 20.0));
			// The hydrogen store starts empty: the backup can only burn
			// what the split delivers, never a pre-loaded buffer.
			stores.push_back(plainStore("h2", 10.0, 0.0));
			core::StationSystemsSettings settings;
			settings.stores = stores;
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			settings.solarArrays = { field };
			core::ElectrolyzerSettings cell;
			cell.id = "electro";
			cell.waterStoreId = "water";
			cell.oxygenStoreId = "o2";
			cell.hydrogenStoreId = "h2";
			cell.thermalClass = "low";
			cell.heatSinkId = "sink";
			cell.rateKgPerS = 0.001;
			cell.specificEnergyKWhPerKgWater = 4.5;
			cell.maxSinkTemperatureC = 60.0;
			settings.electrolyzers = { cell };
			// The backup burns the produced hydrogen and oxygen and returns
			// its product water to the same water store the electrolyzer
			// feeds from: the loop closes on mass, and only energy is lost.
			core::FuelCellSettings backup;
			backup.id = "backup";
			backup.hydrogenStoreId = "h2";
			backup.oxygenStoreId = "o2";
			backup.waterStoreId = "water";
			backup.thermalClass = "low";
			backup.heatSinkId = "sink";
			backup.ratedOutputKW = 10.0;
			backup.efficiency = 0.6;
			backup.commandedOutputKW = 10.0;
			backup.maxSinkTemperatureC = 80.0;
			settings.fuelCells = { backup };
			require(configure(world, settings), "cycle setup refused");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "cycle run failed");
			auto const elapsed = 600.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& backupSnap = snap.fuelCells[0];
			// The cell is hydrogen-limited: it can only burn the hydrogen
			// the split delivered on the preceding ticks (the backup runs
			// ahead of the electrolyzer in the tick), so its output is the
			// split's hydrogen rate times the cell's electrical yield.
			auto const o2Ratio = core::StationElectrolysisOxygenMassPerWaterKg;
			auto const h2Rate = 0.001 * (1.0 - o2Ratio);
			auto const expectedOutput = h2Rate * 0.6 * lhv;
			require(backupSnap.status == core::StationTransferStatus::Partial
				&& approx(backupSnap.lastOutputKW, expectedOutput),
				"the cycle backup did not settle at the hydrogen supply rate");
			// Mass closes around the loop: the cell burns exactly the
			// hydrogen the split delivered on the ticks it ran - one tick
			// less than the full run, because the first tick's hydrogen
			// arrives after the refused cell - and returns the water and
			// oxygen the split took out.
			auto const burnedH2 = backupSnap.hydrogenConsumedKg;
			auto const returnedWater = backupSnap.waterProducedKg;
			require(approx(burnedH2, h2Rate * (elapsed - dt)),
				"the cell did not burn the hydrogen the split delivered");
			require(approx(returnedWater, burnedH2 * (1.0 + o2PerH2)),
				"the returned water does not match the burned reactants");
			require(approx(findStore(snap, "water").currentQualified, 10.0 - 0.001 * dt),
				"the cycle did not return the split's water");
			require(approx(findStore(snap, "o2").currentQualified,
				20.0 + o2Ratio * 0.001 * dt),
				"the cycle did not return the split's oxygen");
			// Energy: the round trip recovers only the cell's share of the
			// chemical energy the dose paid for - the rest is heat at the
			// electrolyzer and at the cell.
			auto const splitKg = 0.001 * elapsed;
			auto const doseKJ = 4.5 * 3600.0 * splitKg;
			auto const recoveredKJ = backupSnap.electricalKJ;
			require(approx(recoveredKJ, expectedOutput * (elapsed - dt)),
				"the recovered electrical energy does not match the cell's yield");
			auto const roundTrip = recoveredKJ / doseKJ;
			require(roundTrip < 0.55 && roundTrip > 0.45,
				"the electrolysis/fuel-cell cycle losses are not what the declared contracts say");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("cycle per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// A sandbox mirror that finds a broken fuel-cell descriptor on its
		// own numbers is contained: the tick fails, the runtime is
		// disabled and no partial result escapes.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"assert(type(fuelCells) == 'table' and #fuelCells == 1, 'fuel cell descriptor crossing lost')\n"
					"assert(fuelCells[1].ratedOutputKW > 0, 'corrupt fuel cell rate crossing')\n"
					"error('corrupt fuel cell descriptor crossing')\n"
					"end";
			});
			core::World corrupt("FuelCellCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = baseStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell") };
			auto const result = corrupt.configureStationSystems(settings, false, &diagnostic);
			require(!result && !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt fuel-cell sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}
}

void registerStationWaterChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"crews", crews});
	checks.push_back({"water", water});
	checks.push_back({"fuelCells", fuelCells});
}
