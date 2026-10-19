#include "StationBiologyChecks.h"
#include "StationRig.h"
#include <limits>

namespace
{
	using namespace smoke::station;

	// The #409 crop check: a crop population grows a dry Living biomass
	// under delivered lamp power, compartment CO2, irrigation water and
	// separately-tracked N/P/K/mineral stores inside a declared growing
	// window and below a declared Growing capacity, and respires day and
	// night. Provisional test biology is deliberately exaggerated so the
	// hour-scale growth and respiration are observable in short headless
	// runs; the organic dry-mass balance is 0.85 and the declared yield
	// satisfies the light-energy cap (2 kg/kWh x 1700 kJ/kg = 3400 <=
	// 3600 kJ per kWh). Every scenario also runs the bundled Lua
	// sandbox, whose own crop audit contains the model if it drifts.
	// The #571 nutrient-conservation check: respiration burns only the
	// organic a population actually holds, so the bound nitrogen,
	// phosphorus, potassium and mineral mass survives every tick instead
	// of being reclassified as fuel as the biomass shrinks. Every
	// constituent a population ever held - its authored start plus
	// everything it drew from the uptake stores - must still be
	// accounted for in exactly one place: living, harvested to a
	// destination, routed to waste on death, booked as uncontained
	// loss, or respired away as organic. The chemical-energy basis is
	// the organic fraction alone, so the energy fixed by growth equals
	// the energy respired as heat plus the energy still carried in the
	// organic matter left alive or harvested.
	void nutrientConservation(smoke::Context const&)
	{
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::max(1e-12, std::abs(expected));
		};
		auto advanceOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return true;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			return false;
		};
		auto organicFractionOf = [](core::ResourceCompositionSettings const& composition)
		{
			return 1.0 - (composition.nutrientNitrogen + composition.phosphorus
				+ composition.potassium + composition.otherMinerals);
		};
		// The closure residual for one population: what came in, minus
		// everything the population accounts for across its sinks. Zero
		// on every constituent means nothing was created or destroyed.
		// The organic term also closes the energy ledger on the organic
		// basis, so a wrong energy basis shows up here too.
		auto closure = [&](core::ResourceCompositionSettings const& composition,
			double initialBiomassKg, double grownKg, double drawnNitrogenKg,
			double drawnPhosphorusKg, double drawnPotassiumKg, double drawnMineralsKg,
			double respiredOrganicKg, double chemicalEnergyFixedKJ, double respirationHeatKJ,
			double energyKJPerKgOrganic,
			std::vector<core::StationBiologicalConstituents> const& sinks)
		{
			double organic = 0.0, nitrogen = 0.0, phosphorus = 0.0, potassium = 0.0, minerals = 0.0;
			for (auto const& sink : sinks)
			{
				organic += sink.organicKg;
				nitrogen += sink.nutrientNitrogenKg;
				phosphorus += sink.phosphorusKg;
				potassium += sink.potassiumKg;
				minerals += sink.mineralsKg;
			}
			auto const initialOrganicKg = initialBiomassKg * organicFractionOf(composition);
			// Organic enters only through photosynthesis: the grown dry
			// mass contributes its organic fraction, drawn from the
			// compartment's carbon dioxide rather than from a store.
			auto const grownOrganicKg = grownKg * organicFractionOf(composition);
			return std::array<double, 6>{
				initialOrganicKg + grownOrganicKg - organic - respiredOrganicKg,
				initialBiomassKg * composition.nutrientNitrogen + drawnNitrogenKg - nitrogen,
				initialBiomassKg * composition.phosphorus + drawnPhosphorusKg - phosphorus,
				initialBiomassKg * composition.potassium + drawnPotassiumKg - potassium,
				initialBiomassKg * composition.otherMinerals + drawnMineralsKg - minerals,
				initialOrganicKg * energyKJPerKgOrganic + chemicalEnergyFixedKJ
					- respirationHeatKJ - organic * energyKJPerKgOrganic,
			};
		};
		auto requireClosed = [](std::array<double, 6> const& residual, std::string const& where)
		{
			std::array<char const*, 6> const names = {"organic", "nitrogen", "phosphorus",
				"potassium", "minerals", "energy"};
			for (size_t i = 0; i < residual.size(); ++i)
			{
				require(std::abs(residual[i]) <= 1e-9,
					(where + ": the " + names[i] + " balance does not close (residual "
						+ std::to_string(residual[i]) + ")").c_str());
			}
		};
		// A nutrient-rich test crop: 80% of its dry mass is bound
		// nutrient and only 20% is respirable organic, so a model that
		// reclassifies nutrient as fuel loses four fifths of the canopy's
		// declared material and this check cannot pass.
		auto richComposition = []()
		{
			core::ResourceCompositionSettings composition;
			composition.moisture = 0.0;
			composition.nutrientNitrogen = 0.5;
			composition.atmosphericNitrogen = 0.0;
			composition.phosphorus = 0.1;
			composition.potassium = 0.1;
			composition.otherMinerals = 0.1;
			composition.energyKJPerKgDry = 1700.0;
			return composition;
		}();
		auto store = [](std::string const& id, double capacity, double starting)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			return settings;
		};
		auto habitat = []()
		{
			core::AtmosphereSettings settings;
			settings.id = "habitat";
			settings.hazardClass = "normal";
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
		auto uptakeStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(store("irrigation", 200.0, 150.0));
			stores.push_back(store("fertN", 50.0, 40.0));
			stores.push_back(store("fertP", 50.0, 40.0));
			stores.push_back(store("fertK", 50.0, 40.0));
			stores.push_back(store("fertMin", 50.0, 40.0));
			return stores;
		};
		auto richCrop = [&]()
		{
			core::CropPopulationSettings settings;
			settings.id = "canopy";
			settings.atmosphereId = "habitat";
			settings.waterStoreId = "irrigation";
			settings.nitrogenStoreId = "fertN";
			settings.phosphorusStoreId = "fertP";
			settings.potassiumStoreId = "fertK";
			settings.mineralsStoreId = "fertMin";
			settings.composition = richComposition;
			settings.growingAreaM2 = 10.0;
			settings.maxBiomassDensityKgPerM2 = 2.0;
			settings.initialBiomassKg = 1.0;
			settings.lampRatedKW = 3.6;
			settings.illuminationProfile.constantLevel = 0.0;
			settings.circulationPowerKW = 0.1;
			settings.growthYieldKgPerKWh = 2.0;
			// Exaggerated test biology, as the starvation checks elsewhere
			// use: the point is that the organic runs out inside a short
			// headless run, not that the rate is plausible.
			settings.respirationPerHour = 600.0;
			settings.transpirationKgPerKgGrowth = 0.0;
			settings.minGrowingTemperatureC = 10.0;
			settings.maxGrowingTemperatureC = 35.0;
			settings.wasteStoreId = "waste";
			return settings;
		};
		auto configureCrop = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::CropPopulationSettings> crops)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { habitat() };
			settings.cropPopulations = std::move(crops);
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 100.0;
			settings.solarArrays = { field };
			return std::make_pair(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic);
		};
		// Assert receiving inventories, not just population output counters (#601).
		auto requireReceived = [&](core::StationSystemsSnapshot const& snap,
			std::string const& id, core::StationBiologicalConstituents const& sent,
			double energyPerOrganicKg)
		{
			auto const found = std::find_if(snap.stores.begin(), snap.stores.end(),
				[&](auto const& s) { return s.id == id; });
			require(found != snap.stores.end(), "missing receiving store");
			auto const& actual = found->actualConstituents;
			auto value = [&](char const* key) { return actual.at(key); };
			requireClosed({value("dryOrganics") - sent.organicKg,
				value("nutrientNitrogen") - sent.nutrientNitrogenKg,
				value("phosphorus") - sent.phosphorusKg,
				value("potassium") - sent.potassiumKg,
				value("otherMinerals") - sent.mineralsKg,
				value("energy") - sent.organicKg * energyPerOrganicKg},
				"receiving store " + id);
		};
		auto combined = [](core::StationBiologicalConstituents a,
			core::StationBiologicalConstituents const& b)
		{
			a.organicKg += b.organicKg;
			a.nutrientNitrogenKg += b.nutrientNitrogenKg;
			a.phosphorusKg += b.phosphorusKg;
			a.potassiumKg += b.potassiumKg;
			a.mineralsKg += b.mineralsKg;
			return a;
		};
		// Starvation preserves the nutrients the crop cannot burn: a dark
		// 1 kg canopy that is 80% bound nutrient respires away only its
		// 0.2 kg of organic matter, then dies with 0.8 kg of nutrient
		// residue that must arrive in the waste store intact. Before the
		// fix the same run respired essentially the whole kilogram and
		// reported the nitrogen as gone.
		{
			core::World world("NutrientStarve", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = uptakeStores();
			stores.push_back(store("waste", 10.0, 0.0));
			stores.back().composition = richComposition;
			auto unit = richCrop();
			auto const [ok, diagnostic] = configureCrop(world, std::move(stores), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 300, "crop nutrient-starvation run"),
				"the crop nutrient-starvation run failed");
			world.pauseSimulation();
			auto const c = world.getStationSystemsSnapshot().cropPopulations[0];
			require(c.dead, "the nutrient-rich canopy did not starve once its organic ran out");
			require(closeTo(c.respirationKg, 0.2, 1e-6),
				"respiration burned past the organic inventory and into the bound nutrients");
			require(closeTo(c.deadResidueToWasteKg, 0.8, 1e-6),
				"the nutrient residue left the canopy in the wrong amount");
			require(closeTo(c.deadResidueToWasteConstituents.nutrientNitrogenKg, 0.5, 1e-6),
				"the canopy's nitrogen did not survive to the waste path");
			require(closeTo(c.deadResidueToWasteConstituents.phosphorusKg, 0.1, 1e-6),
				"the canopy's phosphorus did not survive to the waste path");
			require(closeTo(c.deadResidueToWasteConstituents.potassiumKg, 0.1, 1e-6),
				"the canopy's potassium did not survive to the waste path");
			require(closeTo(c.deadResidueToWasteConstituents.mineralsKg, 0.1, 1e-6),
				"the canopy's minerals did not survive to the waste path");
			requireReceived(world.getStationSystemsSnapshot(), "waste",
				c.deadResidueToWasteConstituents, c.energyKJPerKgOrganic);
			requireClosed(closure(richComposition, unit.initialBiomassKg, c.growthKg,
				c.nitrogenDrawnKg, c.phosphorusDrawnKg, c.potassiumDrawnKg, c.mineralsDrawnKg,
				c.respirationKg, c.chemicalEnergyFixedKJ, c.respirationHeatKJ,
				c.energyKJPerKgOrganic,
				{c.livingConstituents, c.harvestedToFoodConstituents,
					c.harvestedToWasteConstituents, c.deadResidueToWasteConstituents,
					c.deadResidueLostConstituents}),
				"the starved crop nutrient ledger");
			// #703: retained residue has a different blend from the store's
			// declaration after organics respire. Graph the committed blend,
			// not the nominal fractions multiplied by remaining wet mass.
			auto const histories = world.getStationSystemsMetricHistory();
			for (auto key : core::StationMassConstituents)
			{
				auto const id = std::string("store/waste/constituent/") + key;
				auto const h = std::find_if(histories.begin(), histories.end(), [&](auto const& entry) {
					return entry.descriptor.id == id;
				});
				require(h != histories.end() && !h->samples.empty() && h->samples.back().actual,
					"retained residue constituent observation missing");
				auto const snap = world.getStationSystemsSnapshot();
				auto const owner = std::find_if(snap.stores.begin(), snap.stores.end(), [](auto const& entry) {
					return entry.id == "waste";
				});
				require(closeTo(*h->samples.back().actual, owner->actualConstituents.at(key), 1e-8),
					"constituent history relabelled deposited residue as nominal composition");
			}
		}
		// The same starvation with a full waste path: the residue cannot
		// all be routed, and the remainder must be booked as explicit
		// uncontained loss on the actual constituent inventory, not on
		// the declared mix. The ledger still closes across waste and loss.
		{
			core::World world("NutrientStarveFullWaste", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = uptakeStores();
			stores.push_back(store("waste", 0.3, 0.0));
			stores.back().composition = richComposition;
			auto unit = richCrop();
			auto const [ok, diagnostic] = configureCrop(world, std::move(stores), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 300, "crop full-waste starvation run"),
				"the crop full-waste starvation run failed");
			world.pauseSimulation();
			auto const c = world.getStationSystemsSnapshot().cropPopulations[0];
			require(c.dead, "the nutrient-rich canopy did not starve once its organic ran out");
			require(closeTo(c.respirationKg, 0.2, 1e-6),
				"respiration burned past the organic inventory and into the bound nutrients");
			require(closeTo(c.deadResidueToWasteKg, 0.3, 1e-6),
				"the waste path did not take everything its headroom allowed");
			require(closeTo(c.deadResidueLostKg, 0.5, 1e-6),
				"the unroutable residue was not booked as explicit loss");
			// The loss carries the canopy's real constituent proportions,
			// not the declared composition: 0.5/0.8 of the residue's 0.5 kg
			// of nitrogen is lost, the rest survives in the waste store.
			require(closeTo(c.deadResidueLostConstituents.nutrientNitrogenKg, 0.3125, 1e-6),
				"the uncontained loss did not carry the actual nitrogen share");
			require(closeTo(c.deadResidueToWasteConstituents.nutrientNitrogenKg, 0.1875, 1e-6),
				"the routed residue did not carry the actual nitrogen share");
			requireClosed(closure(richComposition, unit.initialBiomassKg, c.growthKg,
				c.nitrogenDrawnKg, c.phosphorusDrawnKg, c.potassiumDrawnKg, c.mineralsDrawnKg,
				c.respirationKg, c.chemicalEnergyFixedKJ, c.respirationHeatKJ,
				c.energyKJPerKgOrganic,
				{c.livingConstituents, c.harvestedToFoodConstituents,
					c.harvestedToWasteConstituents, c.deadResidueToWasteConstituents,
					c.deadResidueLostConstituents}),
				"the full-waste starved crop nutrient ledger");
			requireReceived(world.getStationSystemsSnapshot(), "waste",
				c.deadResidueToWasteConstituents, c.energyKJPerKgOrganic);
		}
		// A growing, harvested canopy closes the same ledger across every
		// destination at once: the living stand, the food store, the waste
		// store and the respiration that ran between harvests.
		{
			core::World world("NutrientHarvest", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = uptakeStores();
			stores.push_back(store("food", 50.0, 0.0));
			stores.back().composition = richComposition;
			stores.push_back(store("waste", 50.0, 0.0));
			stores.back().composition = richComposition;
			auto unit = richCrop();
			unit.illuminationProfile.constantLevel = 1.0;
			unit.transpirationKgPerKgGrowth = 0.0;
			unit.foodStoreId = "food";
			unit.wasteStoreId = "waste";
			unit.edibleFraction = 0.6;
			unit.harvestTriggerKg = 0.5;
			unit.harvestFraction = 1.0;
			auto const [ok, diagnostic] = configureCrop(world, std::move(stores), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 900, "crop harvest nutrient run"),
				"the crop harvest nutrient run failed");
			world.pauseSimulation();
			auto const c = world.getStationSystemsSnapshot().cropPopulations[0];
			require(c.harvestedKg > 0.0, "the canopy never harvested");
			requireClosed(closure(richComposition, unit.initialBiomassKg, c.growthKg,
				c.nitrogenDrawnKg, c.phosphorusDrawnKg, c.potassiumDrawnKg, c.mineralsDrawnKg,
				c.respirationKg, c.chemicalEnergyFixedKJ, c.respirationHeatKJ,
				c.energyKJPerKgOrganic,
				{c.livingConstituents, c.harvestedToFoodConstituents,
					c.harvestedToWasteConstituents, c.deadResidueToWasteConstituents,
					c.deadResidueLostConstituents}),
				"the harvested crop nutrient ledger");
			// The harvested yield is nutrient-richer than the declared
			// composition, because respiration has been removing organic
			// while the nutrients stayed. Routing the declared mix instead
			// of the actual inventory would put too little nitrogen into
			// the product and leave the difference unaccounted.
			auto const harvestedTotal = c.harvestedToFoodConstituents.organicKg
				+ c.harvestedToFoodConstituents.nutrientNitrogenKg
				+ c.harvestedToFoodConstituents.phosphorusKg
				+ c.harvestedToFoodConstituents.potassiumKg
				+ c.harvestedToFoodConstituents.mineralsKg;
			require(harvestedTotal > 0.0,
				"the harvested constituent inventory is empty");
			require(c.harvestedToFoodConstituents.nutrientNitrogenKg
				> harvestedTotal * unit.composition.nutrientNitrogen,
				"the harvested yield did not carry the enriched nutrient share");
			requireReceived(world.getStationSystemsSnapshot(), "food",
				c.harvestedToFoodConstituents, c.energyKJPerKgOrganic);
			requireReceived(world.getStationSystemsSnapshot(), "waste",
				combined(c.harvestedToWasteConstituents, c.deadResidueToWasteConstituents),
				c.energyKJPerKgOrganic);
		}
		// The same closure for an algae culture, which runs the identical
		// biology on its own state. A dark, nutrient-rich culture burns
		// only its organic share and dies with its nutrients intact in the
		// waste path.
		auto richCulture = [&]()
		{
			core::AlgaeCultureSettings settings;
			settings.id = "pond";
			settings.atmosphereId = "habitat";
			settings.waterStoreId = "makeup";
			settings.nitrogenStoreId = "fertN";
			settings.phosphorusStoreId = "fertP";
			settings.potassiumStoreId = "fertK";
			settings.mineralsStoreId = "fertMin";
			settings.composition = richComposition;
			settings.cultureVolumeL = 1000.0;
			settings.maxBiomassDensityKgPerL = 0.002;
			settings.initialBiomassKg = 1.0;
			settings.lampRatedKW = 36.0;
			settings.illuminationProfile.constantLevel = 0.0;
			settings.circulationPowerKW = 0.1;
			settings.growthYieldKgPerKWh = 2.0;
			settings.respirationPerHour = 600.0;
			settings.evaporationKgPerKgGrowth = 0.0;
			settings.minGrowingTemperatureC = 10.0;
			settings.maxGrowingTemperatureC = 35.0;
			settings.foodStoreId = "afood";
			settings.recoveryStoreId = "arecovery";
			settings.wasteStoreId = "awaste";
			settings.foodFraction = 0.4;
			settings.recoveryFraction = 0.3;
			return settings;
		};
		auto configureAlgae = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::AlgaeCultureSettings> cultures)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { habitat() };
			settings.algaeCultures = std::move(cultures);
			for (auto& s : settings.stores)
				if (s.id == "afood" || s.id == "arecovery" || s.id == "awaste")
					s.composition = richComposition;
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 100.0;
			settings.solarArrays = { field };
			return std::make_pair(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic);
		};
		auto algaeSinks = [](core::StationAlgaeCultureSnapshot const& c)
		{
			return std::vector<core::StationBiologicalConstituents>{
				c.livingConstituents, c.harvestedToFoodConstituents,
				c.harvestedToRecoveryConstituents, c.harvestedToWasteConstituents,
				c.deadResidueToWasteConstituents, c.deadResidueLostConstituents};
		};
		{
			core::World world("AlgaeNutrientStarve", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = uptakeStores();
			stores[0].id = "makeup";
			stores.push_back(store("afood", 50.0, 0.0));
			stores.push_back(store("arecovery", 50.0, 0.0));
			stores.push_back(store("awaste", 50.0, 0.0));
			auto unit = richCulture();
			auto const [ok, diagnostic] = configureAlgae(world, std::move(stores), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 300, "algae nutrient-starvation run"),
				"the algae nutrient-starvation run failed");
			world.pauseSimulation();
			auto const c = world.getStationSystemsSnapshot().algaeCultures[0];
			require(c.dead, "the nutrient-rich culture did not starve once its organic ran out");
			require(closeTo(c.respirationKg, 0.2, 1e-6),
				"algae respiration burned past the organic inventory and into the bound nutrients");
			require(closeTo(c.deadResidueToWasteConstituents.nutrientNitrogenKg, 0.5, 1e-6),
				"the culture's nitrogen did not survive to the waste path");
			require(closeTo(c.deadResidueToWasteConstituents.phosphorusKg, 0.1, 1e-6),
				"the culture's phosphorus did not survive to the waste path");
			require(closeTo(c.deadResidueToWasteConstituents.potassiumKg, 0.1, 1e-6),
				"the culture's potassium did not survive to the waste path");
			require(closeTo(c.deadResidueToWasteConstituents.mineralsKg, 0.1, 1e-6),
				"the culture's minerals did not survive to the waste path");
			requireClosed(closure(richComposition, unit.initialBiomassKg, c.growthKg,
				c.nitrogenDrawnKg, c.phosphorusDrawnKg, c.potassiumDrawnKg, c.mineralsDrawnKg,
				c.respirationKg, c.chemicalEnergyFixedKJ, c.respirationHeatKJ,
				c.energyKJPerKgOrganic, algaeSinks(c)),
				"the starved algae nutrient ledger");
			requireReceived(world.getStationSystemsSnapshot(), "awaste",
				c.deadResidueToWasteConstituents, c.energyKJPerKgOrganic);
		}
		// The same starvation with a full waste path: the culture's
		// residue spills past the waste headroom into explicit uncontained
		// loss, routed on the actual constituent inventory. The ledger
		// still closes.
		{
			core::World world("AlgaeNutrientStarveFullWaste", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = uptakeStores();
			stores[0].id = "makeup";
			stores.push_back(store("afood", 50.0, 0.0));
			stores.push_back(store("arecovery", 50.0, 0.0));
			stores.push_back(store("awaste", 0.3, 0.0));
			auto unit = richCulture();
			auto const [ok, diagnostic] = configureAlgae(world, std::move(stores), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 300, "algae full-waste starvation run"),
				"the algae full-waste starvation run failed");
			world.pauseSimulation();
			auto const c = world.getStationSystemsSnapshot().algaeCultures[0];
			require(c.dead, "the nutrient-rich culture did not starve once its organic ran out");
			require(closeTo(c.respirationKg, 0.2, 1e-6),
				"algae respiration burned past the organic inventory and into the bound nutrients");
			require(closeTo(c.deadResidueToWasteKg, 0.3, 1e-6),
				"the waste path did not take everything its headroom allowed");
			require(closeTo(c.deadResidueLostKg, 0.5, 1e-6),
				"the unroutable residue was not booked as explicit loss");
			require(closeTo(c.deadResidueLostConstituents.nutrientNitrogenKg, 0.3125, 1e-6),
				"the uncontained loss did not carry the actual nitrogen share");
			require(closeTo(c.deadResidueToWasteConstituents.nutrientNitrogenKg, 0.1875, 1e-6),
				"the routed residue did not carry the actual nitrogen share");
			requireClosed(closure(richComposition, unit.initialBiomassKg, c.growthKg,
				c.nitrogenDrawnKg, c.phosphorusDrawnKg, c.potassiumDrawnKg, c.mineralsDrawnKg,
				c.respirationKg, c.chemicalEnergyFixedKJ, c.respirationHeatKJ,
				c.energyKJPerKgOrganic, algaeSinks(c)),
				"the full-waste starved algae nutrient ledger");
			requireReceived(world.getStationSystemsSnapshot(), "awaste",
				c.deadResidueToWasteConstituents, c.energyKJPerKgOrganic);
		}
		// A growing, three-way-harvested culture closes the same ledger
		// across all four sinks at once: the living stand, the food store,
		// the recovery store and the waste store, plus the respiration
		// that ran between harvests.
		{
			core::World world("AlgaeNutrientHarvest", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = uptakeStores();
			stores[0].id = "makeup";
			stores.push_back(store("afood", 50.0, 0.0));
			stores.push_back(store("arecovery", 50.0, 0.0));
			stores.push_back(store("awaste", 50.0, 0.0));
			auto unit = richCulture();
			unit.illuminationProfile.constantLevel = 1.0;
			unit.harvestTriggerKg = 0.5;
			unit.harvestFraction = 1.0;
			auto const [ok, diagnostic] = configureAlgae(world, std::move(stores), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 900, "algae harvest nutrient run"),
				"the algae harvest nutrient run failed");
			world.pauseSimulation();
			auto const c = world.getStationSystemsSnapshot().algaeCultures[0];
			require(c.harvestedKg > 0.0, "the culture never harvested");
			require(c.harvestedToRecoveryConstituents.organicKg > 0.0
				&& c.harvestedToWasteConstituents.organicKg > 0.0,
				"the harvest did not reach every declared destination");
			requireClosed(closure(richComposition, unit.initialBiomassKg, c.growthKg,
				c.nitrogenDrawnKg, c.phosphorusDrawnKg, c.potassiumDrawnKg, c.mineralsDrawnKg,
				c.respirationKg, c.chemicalEnergyFixedKJ, c.respirationHeatKJ,
				c.energyKJPerKgOrganic, algaeSinks(c)),
				"the harvested algae nutrient ledger");
			requireReceived(world.getStationSystemsSnapshot(), "afood",
				c.harvestedToFoodConstituents, c.energyKJPerKgOrganic);
			requireReceived(world.getStationSystemsSnapshot(), "arecovery",
				c.harvestedToRecoveryConstituents, c.energyKJPerKgOrganic);
			requireReceived(world.getStationSystemsSnapshot(), "awaste",
				combined(c.harvestedToWasteConstituents, c.deadResidueToWasteConstituents),
				c.energyKJPerKgOrganic);
		}
		// Coupled boundary: respiration-enriched crop/algae material mixes
		// with initial stock, then really feeds a microbial recovery stage.
		// The high-heat cases must hold depleted feed, not fabricate energy
		// or fail the World tick. Harvest cases use a partial harvest and
		// constrained food/recovery headroom to exercise mixed deposits.
		for (bool algae : {false, true})
		for (bool harvest : {false, true})
		for (bool highHeat : {false, true})
		{
			core::World world("CoupledBiologicalBoundary", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = uptakeStores();
			settings.atmospheres = {habitat()};
			std::string const foodId = algae ? "afood" : "food";
			std::string const wasteId = algae ? "awaste" : "waste";
			std::string const feedId = harvest ? (algae ? "arecovery" : foodId) : wasteId;
			for (auto const& id : {foodId, wasteId, std::string("arecovery")})
			{
				auto s = store(id, id == foodId ? 0.6 : 10.0, id == feedId ? 0.4 : 0.0);
				s.composition = richComposition;
				settings.stores.push_back(s);
			}
			auto crop = richCrop();
			crop.foodStoreId = foodId;
			crop.edibleFraction = 0.6;
			crop.harvestTriggerKg = harvest ? 0.5 : 0.0;
			crop.harvestFraction = 0.4;
			auto culture = richCulture();
			culture.waterStoreId = "irrigation";
			culture.harvestTriggerKg = harvest ? 0.5 : 0.0;
			culture.harvestFraction = 0.4;
			if (harvest)
			{
				crop.illuminationProfile.constantLevel = 1.0;
				culture.illuminationProfile.constantLevel = 1.0;
			}
			if (algae) settings.algaeCultures = {culture};
			else settings.cropPopulations = {crop};
			for (auto const* id : {"oxygen", "processWater", "media", "nutrients",
				"recoveredWater", "co2", "residue"})
				settings.stores.push_back(store(id, 100.0,
					std::string(id) == "oxygen" || std::string(id) == "processWater"
						|| std::string(id) == "media" ? 10.0 : 0.0));
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "low";
			sink.heatCapacityKJPerK = 10000.0;
			sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = -50.0; sink.maxTemperatureC = 200.0;
			settings.thermalReservoirs = {sink};
			core::SolarArraySettings solar;
			solar.id = "solar"; solar.capacityKW = 100.0;
			settings.solarArrays = {solar};
			core::MicrobialProcessorSettings proc;
			proc.id = "digester"; proc.hazardClass = "normal";
			std::string const processorFeedId = harvest ? "buffer" : feedId;
			if (harvest)
			{
				auto buffer = store("buffer", 10.0, 0.0);
				buffer.composition = richComposition;
				settings.stores.push_back(buffer);
				settings.transfers = {{"moveBlend", feedId, "buffer", "kg", 0.1, 0}};
			}
			proc.feedStoreId = processorFeedId; proc.oxygenStoreId = "oxygen";
			proc.waterStoreId = "processWater"; proc.consumableStoreId = "media";
			proc.nutrientStoreId = "nutrients"; proc.recoveredWaterStoreId = "recoveredWater";
			proc.co2StoreId = "co2"; proc.residueStoreId = "residue";
			proc.thermalClass = "low"; proc.heatSinkId = "sink";
			proc.rateKgPerS = 0.2; proc.specificEnergyKWhPerKgFeed = 0.01;
			proc.oxygenPerKgFeedKg = 0.0; proc.processWaterPerKgFeedKg = 0.0;
			proc.co2PerKgFeedKg = 0.0; proc.consumablePerKgFeed = 0.0;
			proc.nitrogenRecovery = 0.8; proc.phosphorusRecovery = 0.7;
			proc.potassiumRecovery = 0.6; proc.mineralsRecovery = 0.5;
			proc.reactionHeatKJPerKgFeed = highHeat ? 1700.0 : 0.0;
			proc.minOperatingTemperatureC = 15.0; proc.maxSinkTemperatureC = 45.0;
			settings.microbialProcessors = {proc};
			std::string diagnostic;
			auto const configured = world.configureStationSystems(settings, false, &diagnostic);
			require(configured, diagnostic.c_str());
			auto find = [](core::StationSystemsSnapshot const& snap, std::string const& id)
			{
				return *std::find_if(snap.stores.begin(), snap.stores.end(),
					[&](auto const& s) { return s.id == id; });
			};
			auto consumed = std::map<std::string, double>{};
			bool held = false;
			world.resumeSimulation();
			for (int tick = 0; tick < 300; ++tick)
			{
				auto const before = find(world.getStationSystemsSnapshot(), processorFeedId);
				require(advanceOrDiagnose(world, 1, "coupled biological boundary"),
					"coupled biological boundary failed");
				auto const snap = world.getStationSystemsSnapshot();
				auto const& p = snap.microbialProcessors[0];
				if (before.currentQualified > 0.0)
					for (auto const& [key, value] : before.actualConstituents)
						consumed[key] += value * p.lastFeedConsumedKg
							/ (before.currentQualified + before.currentUnqualified);
				held = held || p.blockReason.find("chemical energy") != std::string::npos;
			}
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& p = snap.microbialProcessors[0];
			if (!highHeat || !harvest)
				require(p.feedConsumedKg > 0.0, "coupled digester consumed no material");
			requireClosed({0.0,
				p.nitrogenRecoveredKg - consumed["nutrientNitrogen"] * proc.nitrogenRecovery,
				p.phosphorusRecoveredKg - consumed["phosphorus"] * proc.phosphorusRecovery,
				p.potassiumRecoveredKg - consumed["potassium"] * proc.potassiumRecovery,
				p.mineralsRecoveredKg - consumed["otherMinerals"] * proc.mineralsRecovery,
				0.0}, "downstream recovery of the actual blend");
			require(p.reactionHeatKJ <= consumed["energy"] + 1e-8,
				"digestion released more energy than its actual feed carried");
			if (highHeat) require(held, "depleted feed was not held for insufficient chemical energy");
			auto sent = core::StationBiologicalConstituents{};
			double energyPerOrganicKg;
			if (algae)
			{
				auto const& c = snap.algaeCultures[0];
				sent = combined(combined(c.harvestedToFoodConstituents,
					c.harvestedToRecoveryConstituents),
					combined(c.harvestedToWasteConstituents, c.deadResidueToWasteConstituents));
				energyPerOrganicKg = c.energyKJPerKgOrganic;
			}
			else
			{
				auto const& c = snap.cropPopulations[0];
				sent = combined(c.harvestedToFoodConstituents,
					combined(c.harvestedToWasteConstituents, c.deadResidueToWasteConstituents));
				energyPerOrganicKg = c.energyKJPerKgOrganic;
			}
			auto remaining = consumed;
			if (harvest)
				for (auto const& [key, value] : find(snap, "buffer").actualConstituents)
					remaining[key] += value;
			for (auto const& id : {foodId, wasteId, std::string("arecovery")})
				for (auto const& [key, value] : find(snap, id).actualConstituents)
					remaining[key] += value;
			requireClosed({remaining["dryOrganics"] - 0.08 - sent.organicKg,
				remaining["nutrientNitrogen"] - 0.2 - sent.nutrientNitrogenKg,
				remaining["phosphorus"] - 0.04 - sent.phosphorusKg,
				remaining["potassium"] - 0.04 - sent.potassiumKg,
				remaining["otherMinerals"] - 0.04 - sent.mineralsKg,
				remaining["energy"] - 680.0 - sent.organicKg * energyPerOrganicKg},
				"population -> mixed store -> digester boundary");
			require(closeTo(find(snap, "nutrients").currentQualified,
				p.nitrogenRecoveredKg + p.phosphorusRecoveredKg
					+ p.potassiumRecoveredKg + p.mineralsRecoveredKg, 1e-6),
				"recovered constituents did not reach the downstream nutrient store");

			if (harvest && highHeat && !algae)
			{
				settings.microbialProcessors[0].rateKgPerS = 0.0;
				settings.transfers.clear();
				for (auto& s : settings.stores)
				{
					if (s.id == foodId) s.startingQualified = 0.0;
				}
				auto crewWater = store("crewWater", 10.0, 5.0);
				core::ResourceCompositionSettings waterComposition;
				waterComposition.moisture = 0.999;
				crewWater.composition = waterComposition;
				settings.stores.push_back(crewWater);
				for (auto const* id : {"urine", "crewWaste"})
				{
					auto s = store(id, 10.0, 0.0);
					s.composition = richComposition;
					settings.stores.push_back(s);
				}
				core::CrewSettings crew;
				crew.id = "crew"; crew.crewCount = 1; crew.atmosphereId = "habitat";
				crew.foodStoreId = foodId; crew.waterStoreId = "crewWater";
				crew.urineStoreId = "urine"; crew.organicWasteStoreId = "crewWaste";
				crew.foodPerCrewPerS = 0.1; crew.metabolicHeatPerCrewKW = 170.0;
				settings.crews = {crew};
				core::World crewWorld("HoldDepletedCrewFood", 2, 2);
				crewWorld.pauseSimulation(); crewWorld.finishBuild();
				auto const crewConfigured = crewWorld.configureStationSystems(settings, false, &diagnostic);
				require(crewConfigured, diagnostic.c_str());
				crewWorld.resumeSimulation();
				require(advanceOrDiagnose(crewWorld, 300, "depleted crew food"),
					"depleted crew food broke the World tick");
				auto const crewSnap = crewWorld.getStationSystemsSnapshot();
				require(crewSnap.crews[0].foodConsumed == 0.0 && crewSnap.crews[0].heatKJ == 0.0,
					"crew fabricated metabolic heat from depleted food");
				require(find(crewSnap, foodId).currentQualified > 0.0,
					"crew hold fixture never received harvested food");
				requireReceived(crewSnap, foodId, crewSnap.cropPopulations[0].harvestedToFoodConstituents,
					energyPerOrganicKg);
			}

			// An unchanged fixed-yield conversion cannot reinterpret fully
			// respired residue as fresh organic material. Keep it in the feed
			// store with an explicit rejection instead of negative 'losses'.
			if (!harvest && highHeat)
			{
				settings.microbialProcessors[0].rateKgPerS = 0.0;
				for (auto& s : settings.stores)
					if (s.id == feedId) s.startingQualified = 0.0;
				auto product = store("product", 10.0, 0.0);
				product.composition = richComposition;
				settings.stores.push_back(product);
				settings.conversions = {{"fixedRecipe", {{feedId, 0.1}}, {{"product", 0.1}}, 0}};
				core::World conversionWorld("HoldIncompatibleResidue", 2, 2);
				conversionWorld.pauseSimulation(); conversionWorld.finishBuild();
				auto const conversionConfigured = conversionWorld.configureStationSystems(settings, false, &diagnostic);
				require(conversionConfigured, diagnostic.c_str());
				conversionWorld.resumeSimulation();
				require(advanceOrDiagnose(conversionWorld, 300, "incompatible conversion"),
					"incompatible residue broke a conversion tick");
				auto const heldSnap = conversionWorld.getStationSystemsSnapshot();
				require(find(heldSnap, "product").currentQualified == 0.0,
					"fixed conversion fabricated organics/energy from mineral residue");
				require(heldSnap.conversions[0].status == core::StationTransferStatus::Rejected,
					"incompatible conversion was not explicitly rejected");
				requireReceived(heldSnap, feedId, algae
					? heldSnap.algaeCultures[0].deadResidueToWasteConstituents
					: heldSnap.cropPopulations[0].deadResidueToWasteConstituents,
					energyPerOrganicKg);

				// Two consumers reserve the same enriched feed before either
				// exchange commits. Each must spend its share of the real
				// blend, not its share of an already-reserved scalar remainder.
				for (auto const* id : {"consumerA", "consumerB"})
				{
					core::EquipmentLoadSettings load;
					load.id = id; load.electricalDemandKW = 0.1; load.thermalClass = "low";
					load.materialExchanges = {{feedId, 0.02, true}};
					settings.equipmentLoads.push_back(load);
				}
				core::World consumingWorld("ConsumeActualResidue", 2, 2);
				consumingWorld.pauseSimulation(); consumingWorld.finishBuild();
				auto const consumingConfigured = consumingWorld.configureStationSystems(settings, false, &diagnostic);
				require(consumingConfigured, diagnostic.c_str());
				consumingWorld.resumeSimulation();
				require(advanceOrDiagnose(consumingWorld, 300, "competing residue consumers"),
					"competing consumers broke the constituent ledger");
				auto const consumedSnap = consumingWorld.getStationSystemsSnapshot();
				auto const actual = find(consumedSnap, feedId).actualConstituents;
				for (auto const& b : consumedSnap.constituentBalances)
				{
					if (b.constituent == "nutrientNitrogen")
						require(b.lost > 0.0, "external consumers never drew the residue");
					double expected = 0.0;
					if (b.constituent == "nutrientNitrogen") expected = 0.5;
					if (b.constituent == "phosphorus" || b.constituent == "potassium"
						|| b.constituent == "otherMinerals") expected = 0.1;
					require(std::abs(actual.at(b.constituent) + b.lost - expected) < 1e-8,
						"external consumption did not conserve actual residue constituents/energy");
				}
			}
		}
	}

	void crops(smoke::Context const&)
	{
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::max(1e-12, std::abs(expected));
		};
		auto advanceOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return true;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			return false;
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
		auto cropStore = [](std::string const& id, double capacity, double starting)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			return settings;
		};
		auto habitat = []()
		{
			core::AtmosphereSettings settings;
			settings.id = "habitat";
			settings.hazardClass = "normal";
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
		auto crop = [&]()
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
			settings.maxBiomassDensityKgPerM2 = 2.0; // 20 kg Growing capacity
			settings.initialBiomassKg = 1.0;
			settings.lampRatedKW = 3.6;
			settings.illuminationProfile.constantLevel = 1.0;
			settings.circulationPowerKW = 0.1;
			settings.growthYieldKgPerKWh = 2.0;
			settings.respirationPerHour = 0.5;
			settings.transpirationKgPerKgGrowth = 5.0;
			settings.minGrowingTemperatureC = 10.0;
			settings.maxGrowingTemperatureC = 35.0;
			return settings;
		};
		auto fullStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(cropStore("irrigation", 200.0, 150.0));
			stores.push_back(cropStore("fertN", 10.0, 5.0));
			stores.push_back(cropStore("fertP", 10.0, 5.0));
			stores.push_back(cropStore("fertK", 10.0, 5.0));
			stores.push_back(cropStore("fertMin", 10.0, 5.0));
			return stores;
		};
		auto configure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			core::AtmosphereSettings compartment,
			std::vector<core::CropPopulationSettings> populations,
			double solarKW = 100.0)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { compartment };
			settings.cropPopulations = std::move(populations);
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = solarKW;
			settings.solarArrays = { field };
			return std::make_pair(
				world.configureStationSystems(settings, false, &diagnostic), diagnostic);
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		// Refused contracts: every malformed crop declaration is rejected
		// before anything runs, each with its own diagnostic.
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = fullStores();
			settings.atmospheres = { habitat() };
			std::string diagnostic;
			settings.cropPopulations = { crop() };
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"the nominal crop contract was refused");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].atmosphereId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown atmosphere compartment") != std::string::npos,
				"a crop growing in an unknown compartment was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].phosphorusStoreId = "fertX";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("phosphorus store 'fertX' does not exist") != std::string::npos,
				"a crop fed from an unknown nutrient store was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].potassiumStoreId = "fertN";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("are the same store") != std::string::npos,
				"a crop sharing one store between two tracked nutrients was accepted");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			settings.stores[2].unit = "L";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("mass-denominated") != std::string::npos,
				"a crop drawing a nutrient in a non-mass unit was accepted");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			settings.cropPopulations[0].composition.moisture = 0.1;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("carries moisture") != std::string::npos,
				"a crop declaring moisture in its dry-mass composition was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].composition.atmosphericNitrogen = 0.1;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("atmospheric nitrogen") != std::string::npos,
				"a crop declaring atmospheric nitrogen in its composition was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].composition.phosphorus = 0.95;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("dry nutrient fractions exceed 1") != std::string::npos,
				"a crop whose declared dry nutrient fractions leave no organic mass was accepted");
			// Uptake store composition purity (#572): the uptake spends the
			// drawn mass exactly as the role's own material, so a compound
			// uptake store must be pure in that role. A wrong-element
			// compound, atmospheric nitrogen in the bound-nitrogen role,
			// a mixed recovered-nutrient product, or a wet compound in a
			// dry nutrient role are all conversions the contract does not
			// own and are refused; plain stores and pure compounds are not.
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings purePhosphorus;
			purePhosphorus.phosphorus = 1.0;
			settings.stores[1].composition = purePhosphorus;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("nitrogen store 'fertN' declares a compound composition that is not pure bound nutrient nitrogen")
					!= std::string::npos,
				"a phosphorus-only compound store was accepted as the crop nitrogen source");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings pureAtmospheric;
			pureAtmospheric.atmosphericNitrogen = 1.0;
			settings.stores[1].composition = pureAtmospheric;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("not pure bound nutrient nitrogen") != std::string::npos,
				"an atmospheric-nitrogen compound store was accepted as the crop bound-nitrogen source");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings mixedRecovered;
			mixedRecovered.nutrientNitrogen = 0.4;
			mixedRecovered.phosphorus = 0.3;
			mixedRecovered.potassium = 0.3;
			settings.stores[1].composition = mixedRecovered;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("not pure bound nutrient nitrogen") != std::string::npos,
				"a mixed recovered-nutrient store was accepted as a pure nitrogen source");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings wetNitrogen;
			wetNitrogen.moisture = 0.2;
			wetNitrogen.nutrientNitrogen = 1.0;
			settings.stores[1].composition = wetNitrogen;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("not pure bound nutrient nitrogen") != std::string::npos,
				"a wet compound store was accepted as a dry nutrient-nitrogen source");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings nutrientSolution;
			nutrientSolution.moisture = 0.99;
			nutrientSolution.nutrientNitrogen = 0.01;
			settings.stores[0].composition = nutrientSolution;
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"compound irrigation water must be accepted by schema; quality gates govern operation (#742)");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings pureNitrogen;
			pureNitrogen.nutrientNitrogen = 1.0;
			settings.stores[1].composition = pureNitrogen;
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"pure-compound uptake stores matching their roles were refused");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].initialBiomassKg = 25.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("exceeds the declared Growing capacity") != std::string::npos,
				"a crop starting fuller than its Growing capacity was accepted");
			// #581: the two authored factors are each validated finite, but
			// their product - the derived Growing capacity - can still
			// overflow to infinity, and an infinite bound passes every
			// upper-bound comparison downstream. The overflowed product is
			// refused; a large but finite product stays valid.
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].growingAreaM2 = 1e308;
			settings.cropPopulations[0].maxBiomassDensityKgPerM2 = 1e308;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("derived Growing capacity overflows to a non-finite magnitude")
					!= std::string::npos,
				"a crop whose area times density product overflows the Growing capacity was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].growingAreaM2 = 1e308;
			settings.cropPopulations[0].maxBiomassDensityKgPerM2 = 10.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("overflows to a non-finite magnitude") != std::string::npos,
				"a crop whose area times density product overflows to infinity was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].growingAreaM2 = 1e154;
			settings.cropPopulations[0].maxBiomassDensityKgPerM2 = 1e154;
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"a large but finite derived Growing capacity of 1e308 kg was refused");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].growthYieldKgPerKWh = 3.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("free source of chemical energy") != std::string::npos,
				"a crop yield fixing more chemical energy than the light carries was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].minGrowingTemperatureC = 40.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("empty growing window") != std::string::npos,
				"a crop with an empty growing window was accepted");
			settings.cropPopulations = { crop(), crop() };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("id is duplicated") != std::string::npos,
				"duplicate crop population ids were accepted");
			// Hazard segregation (#573): the growing compartment is the
			// containment boundary. The reviewed bypass - hazardous
			// irrigation water feeding a normal-class canopy whose food and
			// waste destinations are normal - is refused, and so are
			// hazardous harvest destinations for a normal population.
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			settings.stores[0].hazardClass = "hazardous";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("irrigation water store 'irrigation' is authored hazardous-class")
					!= std::string::npos
				&& diagnostic.find("normal-class compartment") != std::string::npos,
				"a normal canopy irrigated with hazardous water was accepted");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			settings.stores.push_back(cropStore("cropFood", 10.0, 0.0));
			settings.stores.push_back(cropStore("cropWaste", 10.0, 0.0));
			settings.stores[settings.stores.size() - 2].hazardClass = "hazardous";
			settings.cropPopulations[0].harvestTriggerKg = 10.0;
			settings.cropPopulations[0].foodStoreId = "cropFood";
			settings.cropPopulations[0].wasteStoreId = "cropWaste";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food store 'cropFood' is authored hazardous-class")
					!= std::string::npos,
				"a normal canopy harvesting into a hazardous food store was accepted");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			settings.stores.push_back(cropStore("cropFood", 10.0, 0.0));
			settings.stores.push_back(cropStore("cropWaste", 10.0, 0.0));
			settings.stores.back().hazardClass = "hazardous";
			settings.cropPopulations[0].harvestTriggerKg = 10.0;
			settings.cropPopulations[0].foodStoreId = "cropFood";
			settings.cropPopulations[0].wasteStoreId = "cropWaste";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("waste store 'cropWaste' is authored hazardous-class")
					!= std::string::npos,
				"a normal canopy harvesting into a hazardous waste store was accepted");
		}
		// #581 World seam: an overflowed derived capacity is refused by
		// configureStationSystems before any runtime is built, the World
		// keeps its previous (here: empty) station settings untouched, and
		// a subsequent valid configuration still works. A large but
		// finite capacity configures, runs healthy ticks and publishes
		// its finite Growing capacity intact.
		{
			core::World world("CropCapacityOverflow", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto broken = crop();
			broken.growingAreaM2 = 1e308;
			broken.maxBiomassDensityKgPerM2 = 1e308;
			broken.respirationPerHour = 0.0;
			auto const refused = configure(world, fullStores(), habitat(), { broken });
			require(!refused.first
				&& refused.second.find("overflows to a non-finite magnitude")
					!= std::string::npos,
				"an overflowed crop Growing capacity was configured into the World");
			require(world.getStationSystemsSettings().cropPopulations.empty(),
				"the refused crop configuration mutated the World's authored settings");
			auto huge = crop();
			huge.growingAreaM2 = 1e154;
			huge.maxBiomassDensityKgPerM2 = 1e154;
			auto const [ok, secondDiagnostic] = configure(world, fullStores(), habitat(), { huge });
			require(ok, secondDiagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 120, "huge-but-finite capacity crop run"),
				"huge-but-finite capacity crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.healthy && snap.cropPopulations.size() == 1
				&& snap.cropPopulations[0].capacityKg == 1e308
				&& std::isfinite(snap.cropPopulations[0].capacityKg),
				"the large but finite Growing capacity did not publish intact");
		}
		// Darkness: no light, no growth - but respiration runs day and
		// night, burning the declared organic content against the
		// compartment's oxygen.
		{
			core::World world("CropDark", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto unit = crop();
			unit.illuminationProfile.constantLevel = 0.0;
			auto const [ok, diagnostic] = configure(world, fullStores(), habitat(), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "dark crop run"), "dark crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.growthKg == 0.0 && c.co2DrawnKg == 0.0
				&& c.oxygenReleasedKg == 0.0 && c.transpiredKg == 0.0,
				"the canopy grew in darkness");
			require(c.lastLightRequestedKW == 0.0
				&& c.blockReason.find("dark") != std::string::npos,
				"the dark canopy's block reason does not name the darkness");
			require(c.respirationKg > 0.0 && c.respirationO2DrawnKg > 0.0
				&& c.respirationCo2ReturnedKg > 0.0 && c.respirationWaterKg > 0.0,
				"respiration did not run in darkness");
			require(c.livingBiomassKg < 1.0,
				"the dark canopy's living biomass did not burn down");
			require(closeTo(c.respirationO2DrawnKg, c.respirationKg
				* core::StationMolarMassOxygenKgPerMol
				/ core::StationMolarMassCarbohydrateKgPerMol, 1e-9),
				"respiration oxygen draw does not follow the CH2O stoichiometry");
			require(std::abs(c.residualKg) <= 1e-9
				&& std::abs(c.residualKJ) <= 1e-6 * std::max(1.0, c.lightEnergyKJ),
				"the dark canopy's ledgers do not close");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("crop-run boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Full light: growth runs on the delivered light energy, every
		// pathway books, and the compartment gains oxygen and vapour while
		// losing CO2.
		{
			core::World world("CropLight", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto const [ok, diagnostic] = configure(world, fullStores(), habitat(), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "lit crop run"), "lit crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.growthKg > 0.0 && c.co2DrawnKg > 0.0 && c.oxygenReleasedKg > 0.0,
				"the lit canopy did not grow under full light");
			// The growth is exactly the delivered light energy at the
			// declared yield: nothing more, nothing less.
			require(closeTo(c.growthKg, c.lightEnergyKJ * 2.0 / 3600.0, 1e-9),
				"growth does not follow the delivered light energy at the declared yield");
			require(closeTo(c.oxygenReleasedKg, c.co2DrawnKg
				* core::StationMolarMassOxygenKgPerMol
				/ core::StationMolarMassCarbonDioxideKgPerMol, 1e-9),
				"the O2/CO2 exchange does not follow the CH2O stoichiometry");
			require(closeTo(c.transpiredKg, c.growthKg * 5.0, 1e-9),
				"transpiration does not follow the declared per-kg-growth rate");
			require(closeTo(c.waterDrawnKg, c.growthKg * (0.85
				* core::StationMolarMassWaterKgPerMol
				/ core::StationMolarMassCarbohydrateKgPerMol + 5.0), 1e-9),
				"the irrigation draw does not match assimilation water plus transpiration");
			require(closeTo(c.chemicalEnergyFixedKJ, c.growthKg * 1700.0, 1e-9),
				"the fixed chemical energy does not follow the declared dry energy");
			require(c.chemicalEnergyFixedKJ <= c.lightEnergyKJ + 1e-6,
				"the canopy fixed more chemical energy than the light delivered");
			require(c.livingBiomassKg > 1.0,
				"the lit canopy's living biomass did not rise");
			auto const& a = snap.atmospheres[0];
			require(a.carbonDioxideKg < 5.0 && a.oxygenKg > 40.0 && a.waterVapourKg > 1.0,
				"the compartment did not lose CO2 and gain O2 and vapour");
			require(findStore(snap, "irrigation").currentQualified < 150.0
				&& findStore(snap, "fertN").currentQualified < 5.0
				&& findStore(snap, "fertP").currentQualified < 5.0
				&& findStore(snap, "fertK").currentQualified < 5.0
				&& findStore(snap, "fertMin").currentQualified < 5.0,
				"the lit canopy drew no water or nutrients from their stores");
			require(std::abs(c.residualKg) <= 1e-9
				&& std::abs(c.residualKJ) <= 1e-6 * std::max(1.0, c.lightEnergyKJ),
				"the lit canopy's mass/energy ledgers do not close");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("crop-run boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Single-nutrient depletion: a tiny phosphorus store binds the
		// growth scale alone - the abundant nitrogen stays nearly whole -
		// and the limiter names phosphorus.
		{
			core::World world("CropStarvedP", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = fullStores();
			stores[2] = cropStore("fertP", 1.0, 0.0004);
			auto const [ok, diagnostic] = configure(world, std::move(stores), habitat(), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "phosphorus-starved crop run"),
				"phosphorus-starved crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.phosphorusDrawnKg > 0.0003
				&& findStore(snap, "fertP").currentQualified < 1e-9,
				"the scarce phosphorus store was not drawn down");
			require(findStore(snap, "fertN").currentQualified > 4.9,
				"nitrogen was over-drawn while phosphorus was the scarce nutrient");
			require(c.lastGrowthLimiter.find("phosphorus") != std::string::npos,
				"the growth limiter does not name the exhausted phosphorus store");
			require(c.growthKg > 0.0 && c.growthKg < 0.05,
				"growth did not bind to the scarce phosphorus");
		}
		// The #572 reproducer through the public World seam: a store
		// declaring a phosphorus-only compound bound as the crop's
		// nitrogen source is refused at configuration, so no tick can
		// ever grow nitrogen-containing biomass from a source that
		// contains no nitrogen.
		{
			core::World world("CropWrongElement", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = fullStores();
			core::ResourceCompositionSettings purePhosphorus;
			purePhosphorus.phosphorus = 1.0;
			stores[1].composition = purePhosphorus;
			auto const [ok, diagnostic] = configure(world, std::move(stores), habitat(), { crop() });
			require(!ok
				&& diagnostic.find("nitrogen store 'fertN' declares a compound composition that is not pure bound nutrient nitrogen")
					!= std::string::npos,
				"the P-only-compound nitrogen source was accepted through the World seam");
		}
		// Pure-compound uptake with single-element depletion (#572): a
		// store declaring pure bound nutrient nitrogen funds growth 1:1
		// exactly like a plain store, and when it runs dry the growth
		// binds to nitrogen alone - the compartment's 80 kg of
		// atmospheric nitrogen is never touched as a substitute, and
		// every other nutrient is drawn only in its declared proportion
		// of the growth that actually ran.
		{
			core::World world("CropPureCompoundN", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = fullStores();
			core::ResourceCompositionSettings pureNitrogen;
			pureNitrogen.nutrientNitrogen = 1.0;
			stores[1].composition = pureNitrogen;
			stores[1].capacity = 1.0;
			stores[1].startingQualified = 0.0008;
			auto const [ok, diagnostic] = configure(world, std::move(stores), habitat(), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "pure-compound nitrogen crop run"),
				"pure-compound nitrogen crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			// The pure compound funded nitrogen 1:1: every kg drawn is a
			// kg of bound nutrient nitrogen, exactly the declared share of
			// the growth that ran.
			require(closeTo(c.nitrogenDrawnKg, c.growthKg * 0.05, 1e-6),
				"the pure nitrogen compound did not fund the declared nitrogen share 1:1");
			require(c.nitrogenDrawnKg > 0.0007
				&& findStore(snap, "fertN").currentQualified < 1e-9,
				"the scarce pure-nitrogen store was not drawn down");
			require(c.lastGrowthLimiter.find("nitrogen") != std::string::npos,
				"the growth limiter does not name the exhausted nitrogen store");
			// No substitution from the air: the compartment's atmospheric
			// nitrogen inventory is untouched, and the other nutrients
			// follow the growth that actually ran, not the nitrogen.
			require(closeTo(snap.atmospheres[0].nitrogenKg, 80.0, 1e-12),
				"atmospheric nitrogen was drawn to substitute for the exhausted nutrient store");
			require(closeTo(c.phosphorusDrawnKg, c.growthKg * 0.02, 1e-6)
				&& closeTo(c.potassiumDrawnKg, c.growthKg * 0.03, 1e-6)
				&& closeTo(c.mineralsDrawnKg, c.growthKg * 0.05, 1e-6),
				"the other tracked nutrients did not follow their declared growth proportions");
			require(c.growthKg > 0.0 && c.growthKg < 0.02,
				"growth did not bind to the scarce pure nitrogen store");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("pure-compound crop-run boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Capacity saturation: a canopy started near its Growing capacity
		// fills the headroom and then grows no further; the limiter names
		// the capacity.
		{
			core::World world("CropFull", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto unit = crop();
			unit.initialBiomassKg = 19.99;
			unit.respirationPerHour = 0.05; // slow burn so the fill is visible
			auto const [ok, diagnostic] = configure(world, fullStores(), habitat(), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "capacity-bound crop run"),
				"capacity-bound crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.livingBiomassKg <= 20.0 + 1e-9,
				"the canopy grew past its declared Growing capacity");
			require(c.lastGrowthLimiter.find("capacity") != std::string::npos,
				"the growth limiter does not name the full Growing capacity");
			require(c.capacityFraction <= 1.0 + 1e-9 && c.capacityFraction > 0.9,
				"the reported capacity fraction does not track the full canopy");
		}
		// Cold dormancy: below the growing window the canopy grows
		// nothing but still respires; the block reason names the dormancy.
		{
			core::World world("CropCold", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto compartment = habitat();
			compartment.startingTemperatureC = 5.0;
			auto const [ok, diagnostic] = configure(world, fullStores(), compartment, { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "cold crop run"), "cold crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.growthKg == 0.0 && c.co2DrawnKg == 0.0,
				"the cold canopy grew below its growing window");
			require(c.respirationKg > 0.0,
				"respiration stopped in the cold: it must run day and night");
			require(c.blockReason.find("dormant") != std::string::npos
				&& !c.temperatureEligible,
				"the cold canopy's block reason does not name the dormancy");
		}
		// Circulation priority under scarcity: the essential circulation
		// is paid for before the discretionary lamp demand, so a shed
		// lighting budget never starves the pumps.
		{
			core::World world("CropScarcePower", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto unit = crop();
			unit.circulationPowerKW = 0.3;
			auto const [ok, diagnostic] =
				configure(world, fullStores(), habitat(), { unit }, 0.4);
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 60, "power-scarce crop run"),
				"power-scarce crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(closeTo(c.lastCirculationSuppliedKW, 0.3, 1e-9),
				"the essential circulation was shed before the discretionary lamp");
			require(c.lastLightSuppliedKW < 1.0 && c.lastLightSuppliedKW > 0.0,
				"the lamp did not absorb the remaining scarce power");
		}
		// A zero-area population is a declared non-service: it draws no
		// power and grows nothing.
		{
			core::World world("CropZero", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto unit = crop();
			unit.growingAreaM2 = 0.0;
			unit.initialBiomassKg = 0.0;
			auto const [ok, diagnostic] = configure(world, fullStores(), habitat(), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 60, "zero-area crop run"), "zero-area crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.disabled && c.lastLightRequestedKW == 0.0
				&& c.lastCirculationRequestedKW == 0.0 && c.growthKg == 0.0,
				"the zero-area population drew power or grew");
		}
		// Determinism: two identically-authored canopies advanced
		// identically end identically, field for field.
		{
			core::World first("CropDetA", 2, 2);
			core::World second("CropDetB", 2, 2);
			first.pauseSimulation();
			second.pauseSimulation();
			first.finishBuild();
			second.finishBuild();
			require(configure(first, fullStores(), habitat(), { crop() }).first
				&& configure(second, fullStores(), habitat(), { crop() }).first,
				"the determinism pair failed to configure");
			first.resumeSimulation();
			second.resumeSimulation();
			require(first.advanceTicks(300) && second.advanceTicks(300),
				"the determinism pair failed to advance");
			first.pauseSimulation();
			second.pauseSimulation();
			require(first.getStationSystemsSnapshot().cropPopulations[0]
				== second.getStationSystemsSnapshot().cropPopulations[0],
				"identically-authored canopies diverged");
			require(first.getStationSystemsSnapshot().atmospheres[0]
				== second.getStationSystemsSnapshot().atmospheres[0],
				"the compartments of identically-authored canopies diverged");
		}
	}

	// The #410 harvest check: a crop population's declared harvest rule
	// removes real living dry biomass and preserves its declared
	// constituents into the food and waste stores; full destinations
	// constrain the harvest or route the edible surplus through the
	// waste path; a canopy whose living organic content is respired away
	// dies and its residue is routed to waste or booked as explicit
	// uncontained loss; and a coupled crew eats the harvested food from
	// the same store. Test biology is deliberately exaggerated so the
	// hour-scale growth, respiration and starvation are observable in
	// short headless runs; every scenario also runs the bundled Lua
	// sandbox, whose own harvest and loss audit contains the model if
	// it drifts.
	void harvest(smoke::Context const&)
	{
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::max(1e-12, std::abs(expected));
		};
		auto advanceOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return true;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			return false;
		};
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
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
		auto cropStore = [](std::string const& id, double capacity, double starting,
			std::optional<core::ResourceCompositionSettings> composition = std::nullopt)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.composition = std::move(composition);
			return settings;
		};
		auto habitat = []()
		{
			core::AtmosphereSettings settings;
			settings.id = "habitat";
			settings.hazardClass = "normal";
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
		// A fast-growing test canopy: 0.02 kg/s of dry biomass under full
		// light, so a sub-kilogram harvest trigger is crossed within a
		// short headless run.
		auto crop = [&]()
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
			settings.maxBiomassDensityKgPerM2 = 2.0; // 20 kg Growing capacity
			settings.initialBiomassKg = 0.1;
			settings.lampRatedKW = 36.0;
			settings.illuminationProfile.constantLevel = 1.0;
			settings.circulationPowerKW = 0.1;
			settings.growthYieldKgPerKWh = 2.0;
			settings.respirationPerHour = 0.5;
			settings.transpirationKgPerKgGrowth = 1.0;
			settings.minGrowingTemperatureC = 10.0;
			settings.maxGrowingTemperatureC = 35.0;
			settings.foodStoreId = "food";
			settings.wasteStoreId = "waste";
			settings.edibleFraction = 0.6;
			settings.harvestTriggerKg = 0.3;
			settings.harvestFraction = 1.0;
			return settings;
		};
		auto fullStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(cropStore("irrigation", 200.0, 150.0));
			stores.push_back(cropStore("fertN", 10.0, 5.0));
			stores.push_back(cropStore("fertP", 10.0, 5.0));
			stores.push_back(cropStore("fertK", 10.0, 5.0));
			stores.push_back(cropStore("fertMin", 10.0, 5.0));
			stores.push_back(cropStore("food", 10.0, 0.0));
			stores.push_back(cropStore("waste", 10.0, 0.0));
			return stores;
		};
		auto configure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::CropPopulationSettings> populations,
			std::vector<core::CrewSettings> crews = {},
			double solarKW = 100.0)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { habitat() };
			settings.cropPopulations = std::move(populations);
			settings.crews = std::move(crews);
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = solarKW;
			settings.solarArrays = { field };
			return std::make_pair(
				world.configureStationSystems(settings, false, &diagnostic), diagnostic);
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		// Refused harvest contracts: every malformed harvest declaration is
		// rejected before anything runs, each with its own diagnostic.
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = fullStores();
			settings.atmospheres = { habitat() };
			std::string diagnostic;
			settings.cropPopulations = { crop() };
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"the nominal harvest contract was refused");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].foodStoreId = "";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food destination store is not named") != std::string::npos,
				"a harvest enabled without a food destination was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].wasteStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("waste store 'ghost' does not exist") != std::string::npos,
				"a harvest into an unknown waste store was accepted");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			settings.stores[5].unit = "L";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food store 'food' must be a mass unit") != std::string::npos,
				"a harvest into a non-mass food store was accepted");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			settings.cropPopulations[0].wasteStoreId = "food";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food store and waste store are the same store")
					!= std::string::npos,
				"a harvest with one destination for edible and inedible yield was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].edibleFraction = 1.5;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("edible fraction must be finite and from 0 to 1")
					!= std::string::npos,
				"an edible fraction above the harvest was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].harvestFraction = 0.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("a harvest that removes nothing is not a harvest")
					!= std::string::npos,
				"a zero harvest fraction was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].harvestTriggerKg = 25.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("harvest trigger exceeds the declared Growing capacity")
					!= std::string::npos,
				"a harvest trigger above the Growing capacity was accepted");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings different;
			different.moisture = 0.0;
			different.nutrientNitrogen = 0.1;
			different.energyKJPerKgDry = 900.0;
			settings.stores[5].composition = different;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("declares a composition different from the crop's")
					!= std::string::npos,
				"a harvest into a store declaring a different compound material was accepted");
			// A harvest-disabled population (trigger 0) needs no
			// destinations and stays valid.
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].harvestTriggerKg = 0.0;
			settings.cropPopulations[0].foodStoreId = "";
			settings.cropPopulations[0].wasteStoreId = "";
			settings.stores = fullStores();
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"a harvest-disabled crop population was refused");
		}
		// Basic harvest: the canopy crosses its trigger, real living
		// material is removed, and the declared edible/inedible split
		// lands in the food and waste stores with the constituents
		// preserved and every ledger closed.
		{
			core::World world("HarvestBasic", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto const [ok, diagnostic] = configure(world, fullStores(), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "basic harvest run"), "basic harvest run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.growthKg > 0.0, "the canopy did not grow before harvesting");
			require(c.harvestedKg > 0.0, "the harvest never fired");
			require(c.foodProducedKg > 0.0 && c.wasteProducedKg > 0.0,
				"the harvest did not fill both the food and waste streams");
			require(closeTo(c.foodProducedKg + c.wasteProducedKg, c.harvestedKg, 1e-9),
				"the harvested mass does not equal food plus waste");
			require(closeTo(c.foodProducedKg, c.harvestedKg * 0.6, 1e-6),
				"the food stream does not follow the declared edible fraction");
			require(closeTo(findStore(snap, "food").currentQualified, c.foodProducedKg, 1e-9),
				"the food store does not hold the harvested edible mass");
			require(closeTo(findStore(snap, "waste").currentQualified, c.wasteProducedKg, 1e-9),
				"the waste store does not hold the harvested inedible mass");
			require(c.livingBiomassKg < 0.3 + 1e-9,
				"the living biomass stayed above the harvest trigger after harvesting");
			require(std::abs(c.residualKg) <= 1e-9
				&& std::abs(c.residualKJ) <= 1e-6 * std::max(1.0, c.lightEnergyKJ),
				"the harvested canopy's mass/energy ledgers do not close");
			bool foodBalance = false;
			for (auto const& balance : snap.balances)
			{
				require(std::abs(balance.residual) <= 1e-6,
					("harvest-run boundary residual for '" + balance.unit + "' not closed").c_str());
				if (balance.unit == "kg")
				{
					foodBalance = balance.harvested > 0.0;
					require(balance.harvested >= c.harvestedKg - 1e-9,
						"the per-unit harvested term does not account the harvest inflow");
				}
			}
			require(foodBalance, "the balance ledger carries no harvested inflow term");
			require(sawEvent(world, "harvested"),
				"the harvest occurrence was not announced in the event log");
		}
		// Full food destination: the edible surplus is routed through
		// the eligible waste path rather than discarded, and the limiter
		// names the full food store.
		{
			core::World world("HarvestSurplus", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = fullStores();
			stores[5] = cropStore("food", 0.1, 0.0);
			auto const [ok, diagnostic] = configure(world, std::move(stores), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "surplus harvest run"), "surplus harvest run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.surplusEdibleToWasteKg > 0.0,
				"the edible surplus was not routed through the waste path");
			require(findStore(snap, "food").currentQualified <= 0.1 + 1e-9,
				"the food store overflowed its capacity");
			require(c.wasteProducedKg > c.harvestedKg * 0.4 + 1e-9,
				"the waste stream did not carry the routed edible surplus");
			require(std::abs(c.residualKg) <= 1e-9,
				"the surplus-routed harvest's mass ledger does not close");
			require(sawEvent(world, "harvested"),
				"the surplus harvest was not announced");
		}
		// Full food AND full waste: the harvest is constrained - the
		// unplaceable remainder stays living rather than vanishing, and
		// the constraint is announced.
		{
			core::World world("HarvestConstrained", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = fullStores();
			stores[5] = cropStore("food", 0.05, 0.0);
			stores[6] = cropStore("waste", 0.05, 0.0);
			auto const [ok, diagnostic] = configure(world, std::move(stores), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "constrained harvest run"),
				"constrained harvest run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.harvestedKg > 0.0 && c.harvestedKg < 0.11,
				"the harvest was not capped by the tiny destinations");
			require(c.lastHarvestConstrainedKg > 0.0,
				"the constrained remainder is not reported");
			require(c.livingBiomassKg > 0.1,
				"the constrained harvest discarded living material instead of leaving it standing");
			require(std::abs(c.residualKg) <= 1e-9,
				"the constrained harvest's mass ledger does not close");
			require(sawEvent(world, "harvest constrained"),
				"the harvest constraint was not announced in the event log");
		}
		// Starvation death: in darkness the canopy respires its living
		// organic content away and dies; the residue is routed to the
		// waste store, the loss is explicit, and the dead population
		// never grows, respires or harvests again.
		{
			core::World world("HarvestStarved", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto unit = crop();
			unit.illuminationProfile.constantLevel = 0.0;
			unit.initialBiomassKg = 0.001;
			unit.respirationPerHour = 600.0; // exaggerated test biology
			auto const [ok, diagnostic] = configure(world, fullStores(), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 6000, "starvation run"), "starvation run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.dead, "the starved canopy did not die");
			require(c.growthKg == 0.0 && c.co2DrawnKg == 0.0,
				"the starved canopy grew in darkness");
			require(c.respirationKg > 0.0,
				"the starved canopy did not respire its organic content away");
			require(c.deadResidueToWasteKg + c.deadResidueLostKg > 0.0,
				"the dead residue was not accounted");
			require(c.livingBiomassKg <= 1e-9,
				"the dead population still holds living biomass");
			require(c.blockReason.find("dead") != std::string::npos,
				"the dead population's block reason does not name the death");
			require(sawEvent(world, "died"),
				"the starvation death was not announced in the event log");
			// Recovery cannot recreate lost biomass for free: running on
			// changes nothing for the dead population.
			auto const respirationAtDeath = c.respirationKg;
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "post-death run"), "post-death run failed");
			world.pauseSimulation();
			auto const after = world.getStationSystemsSnapshot();
			require(after.cropPopulations[0].dead
				&& after.cropPopulations[0].respirationKg == respirationAtDeath
				&& after.cropPopulations[0].livingBiomassKg <= 1e-9,
				"the dead population revived or kept respiring after death");
		}
		// Death with a full waste store: the residue cannot be contained
		// and is booked as explicit uncontained loss, never silently
		// deleted.
		{
			core::World world("HarvestLostResidue", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto unit = crop();
			unit.illuminationProfile.constantLevel = 0.0;
			unit.initialBiomassKg = 0.001;
			unit.respirationPerHour = 600.0;
			auto stores = fullStores();
			stores[6] = cropStore("waste", 1.0, 1.0); // full: no headroom
			auto const [ok, diagnostic] = configure(world, std::move(stores), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 6000, "lost-residue run"), "lost-residue run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.dead, "the starved canopy did not die");
			require(c.deadResidueToWasteKg == 0.0
				&& c.deadResidueLostKg > 0.0,
				"the dead residue was not booked as explicit uncontained loss");
			require(std::abs(c.residualKg) <= 1e-9,
				"the lost-residue mass ledger does not close");
		}
		// Coupled crew consumption: the harvested food feeds a crew that
		// eats from the same store; the store holds harvest minus crew
		// consumption and both populations' ledgers close together.
		{
			core::World world("HarvestCrew", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			// The crew's food store is the crop's food store: dry, with the
			// crop's declared composition so the harvest may enter it.
			auto stores = fullStores();
			stores[5] = cropStore("food", 10.0, 0.001, cropComposition());
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			stores.push_back(cropStore("crewWater", 10.0, 5.0, waterComposition));
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.02;
			urineComposition.phosphorus = 0.005;
			urineComposition.potassium = 0.01;
			urineComposition.otherMinerals = 0.01;
			urineComposition.energyKJPerKgDry = 500.0;
			stores.push_back(cropStore("urine", 10.0, 0.0, urineComposition));
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 1000.0;
			stores.push_back(cropStore("organicWaste", 10.0, 0.0, wasteComposition));
			core::CrewSettings crew;
			crew.id = "crew";
			crew.atmosphereId = "habitat";
			crew.crewCount = 1;
			crew.activityProfile.constantLevel = 1.0;
			crew.foodStoreId = "food";
			crew.waterStoreId = "crewWater";
			crew.urineStoreId = "urine";
			crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.0005;
			crew.waterPerCrewPerS = 0.001;
			crew.oxygenPerCrewKgPerS = 0.0005;
			crew.co2PerCrewKgPerS = 0.0004;
			crew.moisturePerCrewKgPerS = 0.0002;
			crew.urinePerCrewPerS = 0.0002;
			crew.organicWastePerCrewPerS = 0.0001;
			crew.metabolicHeatPerCrewKW = 0.1;
			auto const [ok, diagnostic] = configure(world, std::move(stores), { crop() }, { crew });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "crew-coupled harvest run"),
				"crew-coupled harvest run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			auto const& crewSnap = snap.crews[0];
			require(c.foodProducedKg > 0.0, "the canopy produced no food for the crew");
			require(crewSnap.foodConsumed > 0.0,
				"the crew consumed no food while the canopy was harvesting");
			// The food store holds exactly what the harvest put in minus
			// what the crew ate: the two populations share one store and
			// neither creates nor destroys food.
			require(closeTo(findStore(snap, "food").currentQualified,
				0.001 + c.foodProducedKg - crewSnap.foodConsumed, 1e-9),
				"the shared food store does not balance harvest against crew consumption");
			require(std::abs(c.residualKg) <= 1e-9,
				"the crew-coupled canopy's mass ledger does not close");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("crew-coupled run boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Persistence: harvest settings round-trip through the World
		// document, and the harvest/death runtime state is never
		// restored - a loaded or Reset world reconstructs the living
		// population from its authored initial condition (#410).
		{
			core::World world("HarvestPersist", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto const [ok, diagnostic] = configure(world, fullStores(), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "harvest persistence run"),
				"harvest persistence run failed");
			world.pauseSimulation();
			require(world.getStationSystemsSnapshot().cropPopulations[0].harvestedKg > 0.0,
				"the persistence fixture did not harvest");
			std::string text = yaml(world);
			require(text.find("harvestTriggerKg: 0.3") != std::string::npos
				&& text.find("foodStoreId: food") != std::string::npos
				&& text.find("wasteStoreId: waste") != std::string::npos,
				"the harvest contract did not travel through the World document");
			require(text.find("harvestedKg") == std::string::npos
				&& text.find("deadResidueLostKg") == std::string::npos,
				"runtime harvest ledgers leaked into the World document");
			auto input = core::YamlSerializer::fromString(text);
			input->deserialize();
			core::SerializationWorkData data;
			core::World loaded("HarvestPersistLoaded", 2, 2);
			loaded.deserialize(*input, data);
			require(loaded.getStationSystemsSettings() == world.getStationSystemsSettings(),
				"harvest settings did not round-trip");
			auto const loadedSnap = loaded.getStationSystemsSnapshot();
			require(loadedSnap.worldTick == 0
				&& loadedSnap.cropPopulations[0].harvestedKg == 0.0
				&& !loadedSnap.cropPopulations[0].dead
				&& loadedSnap.cropPopulations[0].livingBiomassKg == 0.1,
				"the loaded world restored live harvest/death state instead of the authored start");
		}
		// Determinism: two identically-authored harvest worlds advanced
		// identically end identically, field for field.
		{
			core::World first("HarvestDetA", 2, 2);
			core::World second("HarvestDetB", 2, 2);
			first.pauseSimulation();
			second.pauseSimulation();
			first.finishBuild();
			second.finishBuild();
			require(configure(first, fullStores(), { crop() }).first
				&& configure(second, fullStores(), { crop() }).first,
				"the harvest determinism pair failed to configure");
			first.resumeSimulation();
			second.resumeSimulation();
			require(first.advanceTicks(1200) && second.advanceTicks(1200),
				"the harvest determinism pair failed to advance");
			first.pauseSimulation();
			second.pauseSimulation();
			require(first.getStationSystemsSnapshot().cropPopulations[0]
				== second.getStationSystemsSnapshot().cropPopulations[0],
				"identically-authored harvest canopies diverged");
			require(first.getStationSystemsSnapshot().stores
				== second.getStationSystemsSnapshot().stores,
				"the stores of identically-authored harvest worlds diverged");
		}
	}

	// The #411 algae check: a separate algae culture runs the same
	// biological lifecycle as the crops with its own installation limits -
	// capacity from the active culture volume times the supported density
	// per litre, its own coefficients, and a harvest that partitions the
	// removed dry mass into three declared destinations (food, recovery,
	// waste) conservatively, each destination's surplus cascading to the
	// next and what none can hold staying living. Malformed culture and
	// harvest-partition contracts are refused at the boundary; the
	// culture starves independently of the crops; the culture stops at
	// its own capacity; and a crop and an algae culture sharing the same
	// compartment, power pool and nutrient stores compete without either
	// creating, destroying or shadowing the other's material. Test
	// biology is deliberately exaggerated so the hour-scale growth,
	// respiration and starvation are observable in short headless runs;
	// every scenario also runs the bundled Lua sandbox, whose own
	// harvest and loss audit contains the model if it drifts.
	void algae(smoke::Context const&)
	{
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::max(1e-12, std::abs(expected));
		};
		auto advanceOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return true;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			return false;
		};
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto cultureComposition = []()
		{
			core::ResourceCompositionSettings composition;
			composition.moisture = 0.0;
			composition.nutrientNitrogen = 0.06;
			composition.atmosphericNitrogen = 0.0;
			composition.phosphorus = 0.02;
			composition.potassium = 0.03;
			composition.otherMinerals = 0.04;
			composition.energyKJPerKgDry = 1700.0;
			return composition;
		};
		auto cultureStore = [](std::string const& id, double capacity, double starting,
			std::optional<core::ResourceCompositionSettings> composition = std::nullopt)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.composition = std::move(composition);
			return settings;
		};
		auto habitat = []()
		{
			core::AtmosphereSettings settings;
			settings.id = "habitat";
			settings.hazardClass = "normal";
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
		// A fast-growing test culture: 1000 L of active volume at 2 g of
		// dry biomass per litre is a 2 kg installation capacity, and
		// 0.02 kg/s of growth under full light crosses a sub-kilogram
		// harvest trigger within a short headless run.
		auto culture = [&]()
		{
			core::AlgaeCultureSettings settings;
			settings.id = "pond";
			settings.atmosphereId = "habitat";
			settings.waterStoreId = "makeup";
			settings.nitrogenStoreId = "fertN";
			settings.phosphorusStoreId = "fertP";
			settings.potassiumStoreId = "fertK";
			settings.mineralsStoreId = "fertMin";
			settings.composition = cultureComposition();
			settings.cultureVolumeL = 1000.0;
			settings.maxBiomassDensityKgPerL = 0.002; // 2 kg culture capacity
			settings.initialBiomassKg = 0.1;
			settings.lampRatedKW = 36.0;
			settings.illuminationProfile.constantLevel = 1.0;
			settings.circulationPowerKW = 0.1;
			settings.growthYieldKgPerKWh = 2.0;
			settings.respirationPerHour = 0.5;
			settings.evaporationKgPerKgGrowth = 1.0;
			settings.minGrowingTemperatureC = 10.0;
			settings.maxGrowingTemperatureC = 35.0;
			settings.foodStoreId = "afood";
			settings.recoveryStoreId = "arecovery";
			settings.wasteStoreId = "awaste";
			settings.foodFraction = 0.4;
			settings.recoveryFraction = 0.3;
			settings.harvestTriggerKg = 0.3;
			settings.harvestFraction = 1.0;
			return settings;
		};
		auto fullStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(cultureStore("makeup", 200.0, 150.0));
			stores.push_back(cultureStore("fertN", 10.0, 5.0));
			stores.push_back(cultureStore("fertP", 10.0, 5.0));
			stores.push_back(cultureStore("fertK", 10.0, 5.0));
			stores.push_back(cultureStore("fertMin", 10.0, 5.0));
			stores.push_back(cultureStore("afood", 10.0, 0.0));
			stores.push_back(cultureStore("arecovery", 10.0, 0.0));
			stores.push_back(cultureStore("awaste", 10.0, 0.0));
			return stores;
		};
		auto configure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::AlgaeCultureSettings> cultures,
			std::vector<core::CropPopulationSettings> crops = {},
			double solarKW = 100.0)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { habitat() };
			settings.algaeCultures = std::move(cultures);
			settings.cropPopulations = std::move(crops);
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = solarKW;
			settings.solarArrays = { field };
			return std::make_pair(
				world.configureStationSystems(settings, false, &diagnostic), diagnostic);
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		// Refused culture and harvest-partition contracts: every malformed
		// declaration is rejected before anything runs, each with its own
		// diagnostic.
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = fullStores();
			settings.atmospheres = { habitat() };
			std::string diagnostic;
			settings.algaeCultures = { culture() };
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"the nominal algae contract was refused");
			settings.algaeCultures = { culture() };
			settings.algaeCultures[0].foodStoreId = "";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food destination store is not named") != std::string::npos,
				"a harvest enabled without a food destination was accepted");
			settings.algaeCultures = { culture() };
			settings.algaeCultures[0].recoveryStoreId = "";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("recovery destination store is not named") != std::string::npos,
				"a harvest enabled without a recovery destination was accepted");
			settings.algaeCultures = { culture() };
			settings.algaeCultures[0].wasteStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("waste store 'ghost' does not exist") != std::string::npos,
				"a harvest into an unknown waste store was accepted");
			settings.algaeCultures = { culture() };
			settings.stores = fullStores();
			settings.stores[6].unit = "L";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("recovery store 'arecovery' must be a mass unit")
					!= std::string::npos,
				"a harvest into a non-mass recovery store was accepted");
			settings.algaeCultures = { culture() };
			settings.stores = fullStores();
			settings.algaeCultures[0].recoveryStoreId = "afood";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food store and recovery store are the same store")
					!= std::string::npos,
				"a harvest with one destination for two partition shares was accepted");
			settings.algaeCultures = { culture() };
			settings.algaeCultures[0].foodFraction = 0.7;
			settings.algaeCultures[0].recoveryFraction = 0.5;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food and recovery fractions together exceed 1")
					!= std::string::npos,
				"a harvest partition claiming more than 100% of the harvest was accepted");
			settings.algaeCultures = { culture() };
			settings.algaeCultures[0].harvestFraction = 0.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("a harvest that removes nothing is not a harvest")
					!= std::string::npos,
				"a zero harvest fraction was accepted");
			settings.algaeCultures = { culture() };
			settings.algaeCultures[0].harvestTriggerKg = 3.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("harvest trigger exceeds the declared culture capacity")
					!= std::string::npos,
				"a harvest trigger above the culture capacity was accepted");
			// #581: the same product-overflow contract as the crops - the
			// culture volume and the density per litre are each validated
			// finite, but their product, the culture's installation bound,
			// can still overflow to infinity and would then pass every
			// upper-bound comparison. Overflow is refused; a large but
			// finite product stays valid.
			settings.algaeCultures = { culture() };
			settings.algaeCultures[0].cultureVolumeL = 1e308;
			settings.algaeCultures[0].maxBiomassDensityKgPerL = 1e308;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("derived culture capacity overflows to a non-finite magnitude")
					!= std::string::npos,
				"an algae culture whose volume times density product overflows the culture capacity was accepted");
			settings.algaeCultures = { culture() };
			settings.algaeCultures[0].cultureVolumeL = 1e308;
			settings.algaeCultures[0].maxBiomassDensityKgPerL = 10.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("overflows to a non-finite magnitude") != std::string::npos,
				"an algae culture whose volume times density product overflows to infinity was accepted");
			settings.algaeCultures = { culture() };
			settings.algaeCultures[0].cultureVolumeL = 1e154;
			settings.algaeCultures[0].maxBiomassDensityKgPerL = 1e154;
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"a large but finite derived culture capacity of 1e308 kg was refused");
			settings.algaeCultures = { culture() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings different;
			different.moisture = 0.0;
			different.nutrientNitrogen = 0.2;
			different.energyKJPerKgDry = 900.0;
			settings.stores[5].composition = different;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("declares a composition different from the culture's")
					!= std::string::npos,
				"a harvest into a store declaring a different compound material was accepted");
			// Uptake composition purity (#572), the same rule as the crops:
			// the culture spends the drawn mass exactly as the role's own
			// material, so a compound uptake store must be pure in that
			// role for the culture water and every tracked nutrient.
			settings.algaeCultures = { culture() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings purePhosphorus;
			purePhosphorus.phosphorus = 1.0;
			settings.stores[1].composition = purePhosphorus;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("nitrogen store 'fertN' declares a compound composition that is not pure bound nutrient nitrogen")
					!= std::string::npos,
				"a phosphorus-only compound store was accepted as the culture nitrogen source");
			settings.algaeCultures = { culture() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings mixedRecovered;
			mixedRecovered.nutrientNitrogen = 0.4;
			mixedRecovered.phosphorus = 0.3;
			mixedRecovered.otherMinerals = 0.3;
			settings.stores[2].composition = mixedRecovered;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("phosphorus store 'fertP' declares a compound composition that is not pure phosphorus")
					!= std::string::npos,
				"a mixed recovered-nutrient store was accepted as a pure phosphorus source");
			settings.algaeCultures = { culture() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings mineralSolution;
			mineralSolution.moisture = 0.995;
			mineralSolution.otherMinerals = 0.005;
			settings.stores[0].composition = mineralSolution;
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"compound culture water must be accepted by schema; quality gates govern operation (#742)");
			settings.algaeCultures = { culture() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings purePotassium;
			purePotassium.potassium = 1.0;
			settings.stores[3].composition = purePotassium;
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"pure-compound culture uptake stores matching their roles were refused");
			settings.algaeCultures = { culture() };
			settings.algaeCultures[0].initialBiomassKg = 3.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("initial living biomass exceeds the declared culture capacity")
					!= std::string::npos,
				"a culture starting fuller than its installation was accepted");
			// A harvest-disabled culture (trigger 0) needs no
			// destinations and stays valid.
			settings.algaeCultures = { culture() };
			settings.algaeCultures[0].harvestTriggerKg = 0.0;
			settings.algaeCultures[0].foodStoreId = "";
			settings.algaeCultures[0].recoveryStoreId = "";
			settings.algaeCultures[0].wasteStoreId = "";
			settings.stores = fullStores();
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"a harvest-disabled algae culture was refused");
			// Hazard segregation (#573): the culture compartment is the
			// containment boundary; the culture water and every harvest
			// partition destination must carry its hazard class.
			settings.algaeCultures = { culture() };
			settings.stores = fullStores();
			settings.stores[0].hazardClass = "hazardous";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("culture water store 'makeup' is authored hazardous-class")
					!= std::string::npos
				&& diagnostic.find("normal-class compartment") != std::string::npos,
				"a normal culture running on hazardous makeup water was accepted");
			settings.algaeCultures = { culture() };
			settings.stores = fullStores();
			settings.stores[5].hazardClass = "hazardous";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food store 'afood' is authored hazardous-class")
					!= std::string::npos,
				"a normal culture harvesting into a hazardous food store was accepted");
			settings.algaeCultures = { culture() };
			settings.stores = fullStores();
			settings.stores[6].hazardClass = "hazardous";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("recovery store 'arecovery' is authored hazardous-class")
					!= std::string::npos,
				"a normal culture harvesting into a hazardous recovery store was accepted");
			settings.algaeCultures = { culture() };
			settings.stores = fullStores();
			settings.stores[7].hazardClass = "hazardous";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("waste store 'awaste' is authored hazardous-class")
					!= std::string::npos,
				"a normal culture harvesting into a hazardous waste store was accepted");
		}
		// Basic three-way harvest: the culture crosses its trigger, real
		// living material is removed, and the declared food/recovery/waste
		// partition lands in the three stores with the constituents
		// preserved and every ledger closed.
		{
			core::World world("AlgaeBasic", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto const [ok, diagnostic] = configure(world, fullStores(), { culture() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "basic algae harvest run"),
				"basic algae harvest run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.algaeCultures[0];
			require(c.capacityKg > 0.0 && closeTo(c.capacityKg, 2.0, 1e-9),
				"the culture capacity is not the declared volume times density");
			require(c.growthKg > 0.0, "the culture did not grow before harvesting");
			require(c.harvestedKg > 0.0, "the harvest never fired");
			require(c.foodProducedKg > 0.0 && c.recoveryProducedKg > 0.0
				&& c.wasteProducedKg > 0.0,
				"the harvest did not fill all three partition streams");
			require(closeTo(c.foodProducedKg + c.recoveryProducedKg + c.wasteProducedKg,
				c.harvestedKg, 1e-9),
				"the harvested mass does not equal food plus recovery plus waste");
			require(closeTo(c.foodProducedKg, c.harvestedKg * 0.4, 1e-6),
				"the food stream does not follow the declared food fraction");
			require(closeTo(c.recoveryProducedKg, c.harvestedKg * 0.3, 1e-6),
				"the recovery stream does not follow the declared recovery fraction");
			require(closeTo(findStore(snap, "afood").currentQualified, c.foodProducedKg, 1e-9),
				"the food store does not hold the harvested food mass");
			require(closeTo(findStore(snap, "arecovery").currentQualified, c.recoveryProducedKg, 1e-9),
				"the recovery store does not hold the harvested recovery mass");
			require(closeTo(findStore(snap, "awaste").currentQualified, c.wasteProducedKg, 1e-9),
				"the waste store does not hold the harvested waste mass");
			require(c.livingBiomassKg < 0.3 + 1e-9,
				"the living biomass stayed above the harvest trigger after harvesting");
			require(std::abs(c.residualKg) <= 1e-9
				&& std::abs(c.residualKJ) <= 1e-6 * std::max(1.0, c.lightEnergyKJ),
				"the harvested culture's mass/energy ledgers do not close");
			bool harvestBalance = false;
			for (auto const& balance : snap.balances)
			{
				require(std::abs(balance.residual) <= 1e-6,
					("algae harvest-run boundary residual for '" + balance.unit
						+ "' not closed").c_str());
				if (balance.unit == "kg")
				{
					harvestBalance = balance.harvested > 0.0;
					require(balance.harvested >= c.harvestedKg - 1e-9,
						"the per-unit harvested term does not account the algae harvest inflow");
				}
			}
			require(harvestBalance, "the balance ledger carries no algae harvested inflow term");
			require(sawEvent(world, "Algae culture 'pond' harvested"),
				"the algae harvest occurrence was not announced in the event log");
		}
		// Full food destination: the food surplus rides the recovery
		// path rather than being discarded, and the limiter names the
		// full food store.
		{
			core::World world("AlgaeFoodSurplus", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = fullStores();
			stores[5] = cultureStore("afood", 0.05, 0.0);
			auto const [ok, diagnostic] = configure(world, std::move(stores), { culture() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "algae food-surplus run"),
				"algae food-surplus run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.algaeCultures[0];
			require(c.foodSurplusToRecoveryKg > 0.0,
				"the food surplus was not routed through the recovery path");
			require(findStore(snap, "afood").currentQualified <= 0.05 + 1e-9,
				"the food store overflowed its capacity");
			require(c.recoveryProducedKg > c.harvestedKg * 0.3 + 1e-9,
				"the recovery stream did not carry the routed food surplus");
			require(std::abs(c.residualKg) <= 1e-9,
				"the food-surplus harvest's mass ledger does not close");
		}
		// Full food AND full recovery: the recovery surplus rides the
		// waste path, and the limiter names the full recovery store.
		{
			core::World world("AlgaeRecoverySurplus", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = fullStores();
			stores[5] = cultureStore("afood", 0.05, 0.0);
			stores[6] = cultureStore("arecovery", 0.05, 0.0);
			auto const [ok, diagnostic] = configure(world, std::move(stores), { culture() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "algae recovery-surplus run"),
				"algae recovery-surplus run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.algaeCultures[0];
			require(c.recoverySurplusToWasteKg > 0.0,
				"the recovery surplus was not routed through the waste path");
			require(findStore(snap, "arecovery").currentQualified <= 0.05 + 1e-9,
				"the recovery store overflowed its capacity");
			require(c.wasteProducedKg > c.harvestedKg * 0.3 + 1e-9,
				"the waste stream did not carry the routed recovery surplus");
			require(std::abs(c.residualKg) <= 1e-9,
				"the recovery-surplus harvest's mass ledger does not close");
		}
		// All destinations full: the harvest is constrained - the
		// unplaceable remainder stays living rather than vanishing, and
		// the constraint is announced.
		{
			core::World world("AlgaeConstrained", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = fullStores();
			stores[5] = cultureStore("afood", 0.02, 0.0);
			stores[6] = cultureStore("arecovery", 0.02, 0.0);
			stores[7] = cultureStore("awaste", 0.02, 0.0);
			auto const [ok, diagnostic] = configure(world, std::move(stores), { culture() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "algae constrained harvest run"),
				"algae constrained harvest run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.algaeCultures[0];
			require(c.harvestedKg > 0.0 && c.harvestedKg < 0.07,
				"the harvest was not capped by the tiny destinations");
			require(c.lastHarvestConstrainedKg > 0.0,
				"the constrained remainder is not reported");
			require(c.livingBiomassKg > 0.1,
				"the constrained harvest discarded living material instead of leaving it standing");
			require(std::abs(c.residualKg) <= 1e-9,
				"the constrained harvest's mass ledger does not close");
			require(sawEvent(world, "harvest constrained"),
				"the algae harvest constraint was not announced in the event log");
		}
		// #581 World seam: an overflowed derived culture capacity is
		// refused by configureStationSystems before any runtime is built,
		// the World keeps its previous (here: empty) station settings
		// untouched, and a subsequent valid configuration still works. A
		// large but finite capacity configures, runs healthy ticks and
		// publishes its finite culture capacity intact.
		{
			core::World world("AlgaeCapacityOverflow", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto broken = culture();
			broken.cultureVolumeL = 1e308;
			broken.maxBiomassDensityKgPerL = 1e308;
			broken.respirationPerHour = 0.0;
			auto const refused = configure(world, fullStores(), { broken });
			require(!refused.first
				&& refused.second.find("overflows to a non-finite magnitude")
					!= std::string::npos,
				"an overflowed algae culture capacity was configured into the World");
			require(world.getStationSystemsSettings().algaeCultures.empty(),
				"the refused culture configuration mutated the World's authored settings");
			auto huge = culture();
			huge.cultureVolumeL = 1e154;
			huge.maxBiomassDensityKgPerL = 1e154;
			auto const [ok, secondDiagnostic] = configure(world, fullStores(), { huge });
			require(ok, secondDiagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 120, "huge-but-finite capacity culture run"),
				"huge-but-finite capacity culture run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.healthy && snap.algaeCultures.size() == 1
				&& snap.algaeCultures[0].capacityKg == 1e308
				&& std::isfinite(snap.algaeCultures[0].capacityKg),
				"the large but finite culture capacity did not publish intact");
		}
		// Culture capacity: with the harvest disabled the culture grows
		// to its own declared capacity and stops there - the limiter
		// names the culture capacity, the capacity fraction saturates
		// at 1.0 and the living biomass never crosses the installation
		// limit.
		{
			core::World world("AlgaeCapacity", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto full = culture();
			full.harvestTriggerKg = 0.0;
			full.foodStoreId = "";
			full.recoveryStoreId = "";
			full.wasteStoreId = "";
			full.initialBiomassKg = 1.99;
			full.respirationPerHour = 0.05; // slow burn so the fill is visible
			auto const [ok, diagnostic] = configure(world, fullStores(), { full });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "algae capacity run"),
				"algae capacity run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.algaeCultures[0];
			require(c.livingBiomassKg <= 2.0 + 1e-9,
				"the culture grew past its declared culture capacity");
			require(c.capacityFraction <= 1.0 + 1e-9 && c.capacityFraction > 0.9,
				"the reported capacity fraction does not track the full culture");
			require(c.lastGrowthLimiter.find("culture capacity") != std::string::npos
				|| c.blockReason.find("culture capacity") != std::string::npos,
				"the capacity limiter was not named once the culture filled");
			require(std::abs(c.residualKg) <= 1e-9,
				"the capacity-saturated culture's mass ledger does not close");
		}
		// Independent starvation: a dark culture respires its living
		// organic content away and dies while a lit crop in the same
		// compartment keeps growing - the two populations starve and
		// live independently, and the dead culture never grows, respires
		// or harvests again. The mirror case (dark crop, lit culture)
		// shows the same independence the other way.
		{
			core::World world("AlgaeIndependentStarvation", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto dark = culture();
			dark.illuminationProfile.constantLevel = 0.0;
			dark.initialBiomassKg = 0.001;
			dark.respirationPerHour = 600.0; // exaggerated test biology
			auto crop = core::CropPopulationSettings{};
			crop.id = "canopy";
			crop.atmosphereId = "habitat";
			crop.waterStoreId = "makeup";
			crop.nitrogenStoreId = "fertN";
			crop.phosphorusStoreId = "fertP";
			crop.potassiumStoreId = "fertK";
			crop.mineralsStoreId = "fertMin";
			crop.composition = cultureComposition();
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
			crop.foodStoreId = "cfood";
			crop.wasteStoreId = "cwaste";
			crop.edibleFraction = 0.6;
			crop.harvestTriggerKg = 0.3;
			crop.harvestFraction = 1.0;
			auto stores = fullStores();
			stores.push_back(cultureStore("cfood", 10.0, 0.0));
			stores.push_back(cultureStore("cwaste", 10.0, 0.0));
			auto const [ok, diagnostic] = configure(world, std::move(stores), { dark }, { crop });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 6000, "algae independent starvation run"),
				"algae independent starvation run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.algaeCultures[0];
			auto const& g = snap.cropPopulations[0];
			require(c.dead, "the dark culture did not die");
			require(c.growthKg == 0.0 && c.co2DrawnKg == 0.0,
				"the dark culture grew in darkness");
			require(c.respirationKg > 0.0,
				"the dark culture did not respire its organic content away");
			require(c.deadResidueToWasteKg + c.deadResidueLostKg > 0.0,
				"the dead culture's residue was not accounted");
			require(c.blockReason.find("dead") != std::string::npos,
				"the dead culture's block reason does not name the death");
			require(!g.dead && g.growthKg > 0.0,
				"the lit crop stopped growing while the culture starved - the populations are not independent");
			require(sawEvent(world, "Algae culture 'pond' died"),
				"the algae starvation death was not announced in the event log");
			// Recovery cannot recreate lost biomass for free: running on
			// changes nothing for the dead culture.
			auto const respirationAtDeath = c.respirationKg;
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "post-death algae run"),
				"post-death algae run failed");
			world.pauseSimulation();
			auto const after = world.getStationSystemsSnapshot();
			require(after.algaeCultures[0].dead
				&& after.algaeCultures[0].respirationKg == respirationAtDeath
				&& after.algaeCultures[0].livingBiomassKg <= 1e-9,
				"the dead culture revived or kept respiring after death");
		}
		// Combined resource competition: a crop and an algae culture
		// sharing the same compartment, the same power pool and the same
		// nitrogen store compete for them. The shared nitrogen store
		// drains to empty and both populations name it as their limiter;
		// the total nitrogen drawn equals what the store held - neither
		// population creates nutrient or consumes the other's share
		// twice; and both populations' ledgers close together.
		{
			core::World world("AlgaeCompetition", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto sharedN = culture();
			sharedN.harvestTriggerKg = 0.0;
			sharedN.foodStoreId = "";
			sharedN.recoveryStoreId = "";
			sharedN.wasteStoreId = "";
			auto crop = core::CropPopulationSettings{};
			crop.id = "canopy";
			crop.atmosphereId = "habitat";
			crop.waterStoreId = "makeup";
			crop.nitrogenStoreId = "fertN"; // the same store the culture draws
			crop.phosphorusStoreId = "fertP";
			crop.potassiumStoreId = "fertK";
			crop.mineralsStoreId = "fertMin";
			crop.composition = cultureComposition();
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
			crop.foodStoreId = "cfood";
			crop.wasteStoreId = "cwaste";
			crop.edibleFraction = 0.6;
			crop.harvestTriggerKg = 0.3;
			crop.harvestFraction = 1.0;
			auto stores = fullStores();
			stores[1] = cultureStore("fertN", 10.0, 0.02); // scarce shared N
			stores.push_back(cultureStore("cfood", 10.0, 0.0));
			stores.push_back(cultureStore("cwaste", 10.0, 0.0));
			auto const [ok, diagnostic] = configure(world, std::move(stores), { sharedN }, { crop });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "algae/crop competition run"),
				"algae/crop competition run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.algaeCultures[0];
			auto const& g = snap.cropPopulations[0];
			require(findStore(snap, "fertN").currentQualified <= 1e-9,
				"the shared nitrogen store was not drained by the competing populations");
			require(c.nitrogenDrawnKg > 0.0 && g.nitrogenDrawnKg > 0.0,
				"one of the competing populations drew no shared nitrogen at all");
			require(closeTo(c.nitrogenDrawnKg + g.nitrogenDrawnKg, 0.02, 1e-6),
				"the shared nitrogen drawn does not equal what the store held - mass was created or double-counted");
			require(c.growthKg > 0.0 && g.growthKg > 0.0,
				"a competing population never grew while the shared store held nitrogen");
			require(std::abs(c.residualKg) <= 1e-9 && std::abs(g.residualKg) <= 1e-9,
				"the competing populations' mass ledgers do not close");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("algae/crop competition boundary residual for '" + balance.unit
						+ "' not closed").c_str());
			// The two populations keep their own capacities: the crop's
			// 20 kg illuminated-area capacity and the culture's 2 kg
			// culture-volume capacity never merge or shadow each other.
			require(!closeTo(g.capacityKg, c.capacityKg, 1e-9)
				&& closeTo(c.capacityKg, 2.0, 1e-9) && closeTo(g.capacityKg, 20.0, 1e-9),
				"the two populations' capacity bases are not independent");
		}
		// Power competition: one small solar field cannot light both the
		// crop's lamps and the culture's lamps. The essential circulation
		// draws are paid first, the earlier-declared crop's light is paid
		// next, and the culture's lamps run short - the shortfall is
		// reported, never invented around, and the culture still grows on
		// the light it actually received.
		{
			core::World world("AlgaePowerCompetition", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto shared = culture();
			shared.harvestTriggerKg = 0.0;
			shared.foodStoreId = "";
			shared.recoveryStoreId = "";
			shared.wasteStoreId = "";
			auto crop = core::CropPopulationSettings{};
			crop.id = "canopy";
			crop.atmosphereId = "habitat";
			crop.waterStoreId = "makeup";
			crop.nitrogenStoreId = "fertN";
			crop.phosphorusStoreId = "fertP";
			crop.potassiumStoreId = "fertK";
			crop.mineralsStoreId = "fertMin";
			crop.composition = cultureComposition();
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
			crop.foodStoreId = "cfood";
			crop.wasteStoreId = "cwaste";
			crop.edibleFraction = 0.6;
			crop.harvestTriggerKg = 0.0;
			auto stores = fullStores();
			stores.push_back(cultureStore("cfood", 10.0, 0.0));
			stores.push_back(cultureStore("cwaste", 10.0, 0.0));
			// 40 kW against 72.2 kW of combined demand: the crop's full
			// 36.1 kW fits, the culture's lamps run on the remainder.
			auto const [ok, diagnostic] = configure(world, std::move(stores), { shared }, { crop }, 40.0);
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "algae/crop power competition run"),
				"algae/crop power competition run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.algaeCultures[0];
			auto const& g = snap.cropPopulations[0];
			require(closeTo(g.lastLightSuppliedKW, 36.0, 1e-6),
				"the crop's lamps did not receive their full demand ahead of the culture");
			require(c.lastLightSuppliedKW < 36.0 - 1.0,
				"the culture's lamps were not starved by the shared power pool");
			require(c.lastCirculationSuppliedKW + c.lastLightSuppliedKW
				+ g.lastCirculationSuppliedKW + g.lastLightSuppliedKW <= 40.0 + 1e-6,
				"the combined draw exceeded the solar supply - free power appeared");
			require(c.growthKg > 0.0,
				"the culture grew nothing despite receiving partial light - the shortfall was not converted into growth");
			require(std::abs(c.residualKJ) <= 1e-6 * std::max(1.0, c.lightEnergyKJ)
				&& std::abs(g.residualKJ) <= 1e-6 * std::max(1.0, g.lightEnergyKJ),
				"the power-competing populations' energy ledgers do not close");
		}
		// Persistence: the algae contract round-trips through the World
		// document, and the harvest/death runtime state is never
		// restored - a loaded world reconstructs the living culture from
		// its authored initial condition (#411).
		{
			core::World world("AlgaePersist", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto const [ok, diagnostic] = configure(world, fullStores(), { culture() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "algae persistence run"),
				"algae persistence run failed");
			world.pauseSimulation();
			require(world.getStationSystemsSnapshot().algaeCultures[0].harvestedKg > 0.0,
				"the persistence fixture did not harvest");
			std::string text = yaml(world);
			require(text.find("harvestTriggerKg: 0.3") != std::string::npos
				&& text.find("foodStoreId: afood") != std::string::npos
				&& text.find("recoveryStoreId: arecovery") != std::string::npos
				&& text.find("wasteStoreId: awaste") != std::string::npos
				&& text.find("cultureVolumeL: 1000") != std::string::npos,
				"the algae contract did not travel through the World document");
			require(text.find("deadResidueLostKg") == std::string::npos,
				"runtime algae ledgers leaked into the World document");
			auto input = core::YamlSerializer::fromString(text);
			input->deserialize();
			core::SerializationWorkData data;
			core::World loaded("AlgaePersistLoaded", 2, 2);
			loaded.deserialize(*input, data);
			require(loaded.getStationSystemsSettings() == world.getStationSystemsSettings(),
				"algae settings did not round-trip");
			auto const loadedSnap = loaded.getStationSystemsSnapshot();
			require(loadedSnap.worldTick == 0
				&& loadedSnap.algaeCultures[0].harvestedKg == 0.0
				&& !loadedSnap.algaeCultures[0].dead
				&& loadedSnap.algaeCultures[0].livingBiomassKg == 0.1,
				"the loaded world restored live algae harvest/death state instead of the authored start");
			// A pre-89 document carrying `algaeCultures` is refused rather
			// than silently dropping the cultures.
			{
				auto stale = text;
				size_t const position = stale.find("version: 102");
				require(position != std::string::npos, "the fixture document carries no version line");
				stale.replace(position, std::string("version: 102").size(), "version: 88");
				require(stale.find("algaeCultures") != std::string::npos,
					"the fixture document carries no algae cultures to test the guard with");
				bool refused = false;
				try
				{
					auto const input2 = core::YamlSerializer::fromString(stale);
					input2->deserialize();
					core::SerializationWorkData data2;
					core::World staleWorld("AlgaeStale", 2, 2);
					staleWorld.deserialize(*input2, data2);
				}
				catch (std::exception const& error)
				{
					refused = true;
					require(std::string(error.what()).find("schema 89") != std::string::npos,
						"the pre-89 algae refusal does not name the required schema");
				}
				require(refused, "a pre-89 document carrying algae cultures was silently accepted");
			}
		}
		// Determinism: two identically-authored algae worlds advanced
		// identically end identically, field for field.
		{
			core::World first("AlgaeDetA", 2, 2);
			core::World second("AlgaeDetB", 2, 2);
			first.pauseSimulation();
			second.pauseSimulation();
			first.finishBuild();
			second.finishBuild();
			require(configure(first, fullStores(), { culture() }).first
				&& configure(second, fullStores(), { culture() }).first,
				"the algae determinism pair failed to configure");
			first.resumeSimulation();
			second.resumeSimulation();
			require(first.advanceTicks(1200) && second.advanceTicks(1200),
				"the algae determinism pair failed to advance");
			first.pauseSimulation();
			second.pauseSimulation();
			require(first.getStationSystemsSnapshot().algaeCultures[0]
				== second.getStationSystemsSnapshot().algaeCultures[0],
				"identically-authored algae cultures diverged");
			require(first.getStationSystemsSnapshot().stores
				== second.getStationSystemsSnapshot().stores,
				"the stores of identically-authored algae worlds diverged");
		}
	}

	// Hazard segregation on biological pathways (#573). The compartment a
	// population lives in is the containment boundary: a whole-chain
	// hazardous rig - hazardous atmosphere, hazardous feed stores,
	// hazardous harvest destinations - must run and keep every kilogram
	// on the hazardous side. Harvest and crew returns land in
	// hazardous-class stores and never appear as normal material; the
	// reviewed cross-class wirings are refused through the World seam
	// before anything runs and never mutate the World. Heat-only
	// couplings stay allowed: a hazardous compartment thermally coupled
	// to a reservoir exchanges energy, not material, and the hazard
	// rule never refuses it.
	void bioContainment(smoke::Context const&)
	{
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::max(1e-12, std::abs(expected));
		};
		auto advanceOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return true;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			return false;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto store = [](std::string const& id, std::string const& hazard,
			double capacity, double starting,
			std::optional<core::ResourceCompositionSettings> composition = std::nullopt)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.hazardClass = hazard;
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.composition = std::move(composition);
			return settings;
		};
		auto compartment = [](std::string const& id, std::string const& hazard)
		{
			core::AtmosphereSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
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
		// The fast-growing canopy of the harvest rig, wired entirely to
		// hazardous-class stores inside a hazardous compartment: 0.02 kg/s
		// of dry growth crosses the 0.3 kg trigger inside the run, so the
		// harvest seam is actually exercised on the hazardous side.
		auto hazCrop = [&]()
		{
			core::CropPopulationSettings settings;
			settings.id = "canopy";
			settings.atmosphereId = "hazHabitat";
			settings.waterStoreId = "hazIrrigation";
			settings.nitrogenStoreId = "hazFertN";
			settings.phosphorusStoreId = "hazFertP";
			settings.potassiumStoreId = "hazFertK";
			settings.mineralsStoreId = "hazFertMin";
			settings.composition = cropComposition();
			settings.growingAreaM2 = 10.0;
			settings.maxBiomassDensityKgPerM2 = 2.0;
			settings.initialBiomassKg = 0.1;
			settings.lampRatedKW = 36.0;
			settings.illuminationProfile.constantLevel = 1.0;
			settings.circulationPowerKW = 0.1;
			settings.growthYieldKgPerKWh = 2.0;
			settings.respirationPerHour = 0.5;
			settings.transpirationKgPerKgGrowth = 1.0;
			settings.minGrowingTemperatureC = 10.0;
			settings.maxGrowingTemperatureC = 35.0;
			settings.foodStoreId = "hazFood";
			settings.wasteStoreId = "hazWaste";
			settings.edibleFraction = 0.6;
			settings.harvestTriggerKg = 0.3;
			settings.harvestFraction = 1.0;
			return settings;
		};
		auto hazCropStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(store("hazIrrigation", "hazardous", 200.0, 150.0));
			stores.push_back(store("hazFertN", "hazardous", 10.0, 5.0));
			stores.push_back(store("hazFertP", "hazardous", 10.0, 5.0));
			stores.push_back(store("hazFertK", "hazardous", 10.0, 5.0));
			stores.push_back(store("hazFertMin", "hazardous", 10.0, 5.0));
			stores.push_back(store("hazFood", "hazardous", 10.0, 0.0));
			stores.push_back(store("hazWaste", "hazardous", 10.0, 0.0));
			return stores;
		};
		// The crew check's closing compositions, carried on hazardous
		// stores: the element and energy closures are class-agnostic, so
		// the same metabolism runs on the hazardous side.
		auto hazCrewStores = [&]()
		{
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
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(store("hazFood", "hazardous", 100.0, 50.0, foodComposition));
			stores.push_back(store("hazWater", "hazardous", 100.0, 50.0, waterComposition));
			stores.push_back(store("hazUrine", "hazardous", 100.0, 0.0, urineComposition));
			stores.push_back(store("hazOrganicWaste", "hazardous", 100.0, 0.0, wasteComposition));
			return stores;
		};
		auto hazCrew = []()
		{
			core::CrewSettings settings;
			settings.id = "crew";
			settings.atmosphereId = "hazHabitat";
			settings.crewCount = 2;
			settings.activityProfile.constantLevel = 1.0;
			settings.foodStoreId = "hazFood";
			settings.waterStoreId = "hazWater";
			settings.urineStoreId = "hazUrine";
			settings.organicWasteStoreId = "hazOrganicWaste";
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
		// The whole-chain hazardous crop rig runs through the World tick
		// seam: the canopy grows, harvests, and every kilogram of harvest
		// lands as qualified material in hazardous-class stores. Nothing
		// in the snapshot leaves the hazardous class - the harvest never
		// relabels what it moves.
		{
			core::World world("BioHazCrop", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = hazCropStores();
			settings.atmospheres = { compartment("hazHabitat", "hazardous") };
			settings.cropPopulations = { hazCrop() };
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 100.0;
			settings.solarArrays = { field };
			require(world.configureStationSystems(settings, false, &diagnostic),
				("the whole-chain hazardous crop rig was refused: " + diagnostic).c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "hazardous crop run"),
				"hazardous crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.growthKg > 0.0 && c.harvestedKg > 0.0,
				"the hazardous canopy never grew or never harvested");
			for (auto const& entry : snap.stores)
				require(entry.hazardClass == "hazardous",
					("a store left the hazardous chain class: " + entry.id).c_str());
			require(findStore(snap, "hazFood").currentQualified > 0.0
				&& findStore(snap, "hazWaste").currentQualified > 0.0,
				"the hazardous harvest did not fill both hazardous destinations");
			require(closeTo(findStore(snap, "hazFood").currentQualified
				+ findStore(snap, "hazWaste").currentQualified, c.harvestedKg, 1e-9),
				"the hazardous harvest split does not equal the harvested mass");
			require(snap.atmospheres[0].hazardClass == "hazardous",
				"the hazardous compartment lost its class");
		}
		// The hazardous crew chain through the World tick seam: metabolism
		// runs on hazardous feed inside a hazardous compartment and every
		// return - urine, organic waste, respired moisture and CO2 -
		// stays on the hazardous side. Crew returns never relabel
		// hazardous material as normal.
		{
			core::World world("BioHazCrew", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = hazCrewStores();
			settings.atmospheres = { compartment("hazHabitat", "hazardous") };
			settings.crews = { hazCrew() };
			require(world.configureStationSystems(settings, false, &diagnostic),
				("the whole-chain hazardous crew rig was refused: " + diagnostic).c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 120, "hazardous crew run"),
				"hazardous crew run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.crews[0];
			require(live.status == core::StationTransferStatus::Full
				&& live.foodConsumed > 0.0,
				"the hazardous crew did not run at full metabolism");
			require(live.urineProduced > 0.0 && live.organicWasteProduced > 0.0,
				"the hazardous crew returned nothing");
			for (auto const& entry : snap.stores)
				require(entry.hazardClass == "hazardous",
					("a crew stream store left the hazardous class: " + entry.id).c_str());
			require(closeTo(findStore(snap, "hazUrine").currentQualified,
				live.urineProduced, 1e-9)
				&& closeTo(findStore(snap, "hazOrganicWaste").currentQualified,
					live.organicWasteProduced, 1e-9),
				"the hazardous returns did not land in the hazardous return stores");
			require(snap.atmospheres[0].hazardClass == "hazardous",
				"the crew's hazardous compartment lost its class");
		}
		// Heat-only couplings stay allowed without material mixing: a
		// hazardous compartment coupled to a thermal reservoir exchanges
		// energy through the exchanger while the hazard rule keeps its
		// hands off - the reservoir carries no material ledger and no
		// class refusal appears.
		{
			core::World world("BioHeatOnly", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = hazCropStores();
			core::ThermalReservoirSettings sink;
			sink.id = "sink";
			sink.thermalClass = "low";
			sink.heatCapacityKJPerK = 10000.0;
			sink.startingTemperatureC = 10.0;
			sink.minTemperatureC = -50.0;
			sink.maxTemperatureC = 200.0;
			settings.thermalReservoirs = { sink };
			auto habitat = compartment("hazHabitat", "hazardous");
			habitat.reservoirId = "sink";
			habitat.exchangerConductanceKWPerK = 5.0;
			settings.atmospheres = { habitat };
			settings.cropPopulations = { hazCrop() };
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 100.0;
			settings.solarArrays = { field };
			require(world.configureStationSystems(settings, false, &diagnostic),
				("a heat-only coupling of a hazardous compartment was refused: "
					+ diagnostic).c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 300, "heat-only hazardous run"),
				"heat-only hazardous run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			// Heat flowed from the warmer hazardous compartment into the
			// cooler reservoir: energy crossed, and only energy crossed.
			require(snap.thermal.reservoirs[0].temperatureC > 10.0,
				"the heat-only coupling did not warm the reservoir");
			require(snap.atmospheres[0].hazardClass == "hazardous",
				"the heat-only coupling disturbed the compartment's hazard class");
			for (auto const& entry : snap.stores)
				require(entry.hazardClass == "hazardous",
					("a chain store left the hazardous class through a heat-only coupling: "
						+ entry.id).c_str());
		}
		// The Lua sandbox mirror: with the authored settings coherent for
		// the host, a crossing rewrite that flips the compartment's class
		// under the hazardous chain must be refused by the bundled
		// model's own assertion, contained through the integrated
		// construction error path - the sandbox enforces the same
		// boundary the host does, independently.
		{
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled] {
				return std::string("local factory = (function()\n") + bundled
					+ "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"for index = 1, #atmospheres do\n"
					"if atmospheres[index].id == 'hazHabitat' then atmospheres[index].hazardClass = 'normal' end\n"
					"end\n"
					"return factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"end";
			});
			core::World sandboxCrop("BioSandboxCropMirror", 2, 2);
			sandboxCrop.pauseSimulation();
			sandboxCrop.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = hazCropStores();
			settings.atmospheres = { compartment("hazHabitat", "hazardous") };
			settings.cropPopulations = { hazCrop() };
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 100.0;
			settings.solarArrays = { field };
			require(!sandboxCrop.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("authored hazardous-class") != std::string::npos
				&& !sandboxCrop.getStationSystemsSnapshot().enabled,
				("the Lua crop hazard mirror did not refuse the rewritten crossing: "
					+ diagnostic).c_str());
			core::World sandboxCrew("BioSandboxCrewMirror", 2, 2);
			sandboxCrew.pauseSimulation();
			sandboxCrew.finishBuild();
			settings = core::StationSystemsSettings{};
			settings.enabled = true;
			settings.stores = hazCrewStores();
			settings.atmospheres = { compartment("hazHabitat", "hazardous") };
			settings.crews = { hazCrew() };
			require(!sandboxCrew.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("authored hazardous-class") != std::string::npos
				&& !sandboxCrew.getStationSystemsSnapshot().enabled,
				("the Lua crew hazard mirror did not refuse the rewritten crossing: "
					+ diagnostic).c_str());
			core::setStationSystemsSourceLoader({});
		}
		// The reviewed bypasses refused through the World configure seam:
		// hazardous irrigation into a normal canopy and hazardous crew
		// food in a normal habitat never reach a running World, and the
		// refusal leaves the World untouched.
		{
			core::World world("BioCrossRefused", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = hazCropStores();
			settings.atmospheres = { compartment("habitat", "normal") };
			settings.cropPopulations = { hazCrop() };
			settings.cropPopulations[0].atmosphereId = "habitat";
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("hazardous-class") != std::string::npos,
				"cross-class crop wiring reached the World configure seam");
			settings.stores = hazCrewStores();
			settings.atmospheres = { compartment("habitat", "normal") };
			settings.cropPopulations = {};
			settings.crews = { hazCrew() };
			settings.crews[0].atmosphereId = "habitat";
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("hazardous-class") != std::string::npos,
				"cross-class crew wiring reached the World configure seam");
			require(!world.getStationSystemsSnapshot().enabled
				&& world.getSimulationTick() == 0,
				"a refused biological hazard configuration mutated the World");
		}
	}
}

void registerStationBiologyChecks(std::vector<smoke::Check>& checks)
{
	checks.insert(checks.end(), {
		{"crops", crops},
		{"harvest", harvest},
		{"algae", algae},
		{"nutrientConservation", nutrientConservation},
		{"bioContainment", bioContainment}});
}
