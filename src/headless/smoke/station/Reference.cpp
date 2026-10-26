#include "StationReferenceFixtures.h"
#include "StationRig.h"
// #422: the packaged reference station.
//
// These checks drive the one reconciled reference configuration packaged
// as an application resource through the public World tick/snapshot/
// event/diagnostic seam: the bundled resource parses and validates, the
// normal-operation scenario runs with its accounting closed, the
// configuration persists and resets like any other authored World, an
// intentional operational shortage is accepted while a physically
// inconsistent edit is refused, and the whole run is deterministic
// across reset and document round trips.
#include "Reference.h"

#include "core/AgentTagRegistryDocument.h"
#include "core/StationReference.h"
#include "core/World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace
{
	void require(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }

	auto const closeTo = [](double actual, double expected, double relative = 1e-6)
	{
		return std::abs(actual - expected) <= relative * std::max(1e-12, std::abs(expected));
	};

	auto storeSnap = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.stores)
			if (entry.id == id) return entry;
		return core::StationSystemsStoreSnapshot{};
	};
	auto atmosSnap = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.atmospheres)
			if (entry.id == id) return entry;
		return core::StationAtmosphereSnapshot{};
	};
	auto firstOf = []<typename T>(std::vector<T> const& items) -> T
	{
		return items.empty() ? T{} : items.front();
	};
	auto findLoad = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.power.loads)
			if (entry.id == id) return entry;
		return core::StationEquipmentLoadSnapshot{};
	};
	auto findDelivery = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.deliveries)
			if (entry.id == id) return entry;
		return core::StationDeliverySnapshot{};
	};
	auto findMaintenance = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.maintenanceJobs)
			if (entry.id == id) return entry;
		return core::StationMaintenanceSnapshot{};
	};
	auto findOutage = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.outages)
			if (entry.id == id) return entry;
		return core::StationOutageSnapshot{};
	};

	auto configureOrDiagnose = [](core::World& world,
		core::StationSystemsSettings const& settings, char const* what)
	{
		std::string diagnostic;
		if (!world.configureStationSystems(settings, false, &diagnostic))
		{
			std::fprintf(stderr, "%s configure failed: %s\n", what, diagnostic.c_str());
			throw std::runtime_error(std::string(what) + " configuration failed");
		}
	};

	auto runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
	{
		if (smoke::station::advanceSeconds(world, static_cast<double>(ticks) / 60.0)) return;
		auto const diagnostics = world.getStationSystemsDiagnostics();
		std::fprintf(stderr, "%s run failed: %s\n", what,
			diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
		throw std::runtime_error(std::string(what) + " run failed");
	};

	// The documented normal-operation scenario length in fixed ticks.
	auto scenarioTicks = [](double seconds)
	{
		return static_cast<uint64_t>(std::llround(seconds / core::World::getFixedTimestep()));
	};

	// ------------------------------------------------------------------
	// The packaged resource parses, validates and carries its documented
	// provenance, assumptions and qualification scope.
	// ------------------------------------------------------------------
	void referenceBundled(smoke::Context const&)
	{
		auto const& reference = core::bundledStationReference();
		require(!reference.name.empty(), "the bundled reference station has no name");
		require(reference.schemaVersion == 111, "the bundled reference is not on the current station schema");
		require(!reference.summary.empty(), "the bundled reference has no summary");
		require(!reference.provenance.empty(), "the bundled reference records no coefficient provenance");
		require(!reference.qualification.empty(), "the bundled reference declares no qualification limits");
		require(reference.scenarioSeconds > 0.0, "the bundled reference declares no scenario duration");
		// The three explicitly required assumption topics are all documented.
		for (auto const& topic : { "Water makeup and recovery",
			"Hydrogen and carbon balance", "Microbial participation" })
		{
			auto const found = std::any_of(reference.assumptions.begin(), reference.assumptions.end(),
				[&](std::pair<std::string, std::string> const& entry)
				{ return entry.first == topic && !entry.second.empty(); });
			require(found, (std::string("the bundled reference does not document: ") + topic).c_str());
		}
		// Every provenance entry carries value, units and a source.
		for (auto const& entry : reference.provenance)
			require(!entry.value.empty() && !entry.units.empty() && !entry.source.empty(),
				"a reference provenance entry is missing its value, units or source");
		// The settings are ordinary StationSystemsSettings and pass the
		// same validation any authored World configuration must pass.
		require(reference.settings.enabled, "the bundled reference is not authored enabled");
		require(reference.settings.radiators.size() == 3
			&& reference.settings.radiators[0].id == "radProcess" && reference.settings.radiators[0].areaM2 == 6.0
			&& reference.settings.radiators[1].id == "radGen" && reference.settings.radiators[1].areaM2 == 0.5,
			"reference radiator areas did not retain their explicitly recalibrated SI balance (#783)");
		// #808: the bottoming cycle is part of the reconciled register. The
		// engine's hot endpoint is the generator's own waste-heat reservoir
		// and its rejection node carries the engine's service class with a
		// radiator of its own, so the ORC is two components sharing plumbing
		// rather than one fused compound machine.
		require(reference.settings.heatEngines.size() == 1
			&& reference.settings.heatEngines[0].id == "orcBottoming"
			&& reference.settings.heatEngines[0].hotReservoirId == "genSink"
			&& reference.settings.heatEngines[0].rejectionReservoirId == "engineReject",
			"the bundled reference lost its generator-waste-heat bottoming cycle (#808)");
		require(std::any_of(reference.settings.thermalReservoirs.begin(),
			reference.settings.thermalReservoirs.end(),
			[](core::ThermalReservoirSettings const& r)
			{ return r.id == "engineReject" && r.thermalClass == "moderate"; })
			&& std::any_of(reference.settings.radiators.begin(),
			reference.settings.radiators.end(),
			[](core::RadiatorSettings const& r)
			{ return r.id == "radEngineReject" && r.reservoirId == "engineReject"; }),
			"the bottoming cycle's rejection reservoir lost its own radiator (#808)");
		std::string diagnostic;
		require(core::validateStationSystemsSettings(reference.settings, diagnostic),
			("the bundled reference configuration was refused: " + diagnostic).c_str());
		// The register is reconciled: no duplicate store ids and the
		// configuration carries the demonstrated breadth of the system.
		require(reference.settings.stores.size() >= 24,
			"the bundled reference register is unexpectedly small");
		require(!reference.settings.crews.empty() && !reference.settings.atmospheres.empty(),
			"the bundled reference has no crew or compartment");
		require(!reference.settings.microbialProcessors.empty()
			&& !reference.settings.sabatierReactors.empty()
			&& !reference.settings.methaneProcessors.empty(),
			"the bundled reference is missing the carbon/microbial loops");
		// The register carries its own dedicated water-recovery chain:
		// raw buffer, treated fresh, and a retained concentrate stream.
		auto const dedicated = std::count_if(reference.settings.stores.begin(),
			reference.settings.stores.end(),
			[](core::ResourceStoreSettings const& store)
			{ return store.id == "rawWater" || store.id == "fresh"
				|| store.id == "concentrate"; });
		require(dedicated == 3, "the bundled reference has no dedicated recovery chain");
		// The resource text round-trips: reparsing the packaged text
		// reproduces the same settings.
		core::StationReferenceDocument reparsed;
		require(core::parseStationReference(core::bundledStationReferenceText(), reparsed, &diagnostic),
			("re-parsing the packaged reference text failed: " + diagnostic).c_str());
		require(reparsed.settings == reference.settings,
			"re-parsing the packaged reference text produced different settings");
		// #896: the packaged generators must remain a missing-key legacy fixture.
		require(core::bundledStationReferenceText().find("primeMover:") == std::string::npos
			&& !reference.settings.generators.empty(), "reference no longer exercises absent prime mover");
		for (auto const& generator : reference.settings.generators)
			require(generator.primeMover == core::StationGeneratorPrimeMover::Reciprocating,
				"legacy reference did not default to piston");
		for (auto const& [primeMover, label] : {
			std::pair{core::StationGeneratorPrimeMover::Reciprocating, "reciprocating"},
			std::pair{core::StationGeneratorPrimeMover::GasTurbine, "gas-turbine"},
			std::pair{core::StationGeneratorPrimeMover::Stirling, "stirling"}})
		{
			core::World baseline("ReferencePrimeMover", 2, 2), labelled("ReferencePrimeMover", 2, 2);
			baseline.pauseSimulation(); baseline.finishBuild(); labelled.pauseSimulation(); labelled.finishBuild();
			auto changed = reference.settings;
			for (auto& generator : changed.generators) generator.primeMover = primeMover;
			require(baseline.configureStationSystems(reference.settings, false, &diagnostic)
				&& labelled.configureStationSystems(changed, false, &diagnostic), "reference prime mover setup refused");
			baseline.resumeSimulation(); labelled.resumeSimulation();
			require(smoke::station::advanceSeconds(baseline, 30) && smoke::station::advanceSeconds(labelled, 30),
				"reference prime mover run failed");
			auto snapshot = labelled.getStationSystemsSnapshot();
			for (auto& generator : snapshot.power.generators)
			{
				require(generator.primeMover == label, "reference snapshot prime mover missing");
				generator.primeMover = "reciprocating";
			}
			require(snapshot == baseline.getStationSystemsSnapshot(), "prime mover changed reference simulation");
		}
		// #891: retain the reference as a genuine missing-key legacy fixture.
		require(core::bundledStationReferenceText().find("chemistry:") == std::string::npos
			&& !reference.settings.fuelCells.empty(), "reference no longer exercises absent chemistry");
		for (auto const& cell : reference.settings.fuelCells)
			require(cell.chemistry == core::FuelCellChemistry::PEM, "legacy reference did not default to PEM");
		for (auto chemistry : {core::FuelCellChemistry::PEM, core::FuelCellChemistry::Alkaline,
			core::FuelCellChemistry::SolidOxide})
		{
			core::World baseline("ReferenceChemistry", 2, 2), labelled("ReferenceChemistry", 2, 2);
			baseline.pauseSimulation(); baseline.finishBuild(); labelled.pauseSimulation(); labelled.finishBuild();
			auto changed = reference.settings;
			for (auto& cell : changed.fuelCells) cell.chemistry = chemistry;
			require(baseline.configureStationSystems(reference.settings, false, &diagnostic)
				&& labelled.configureStationSystems(changed, false, &diagnostic), "reference chemistry setup refused");
			baseline.resumeSimulation(); labelled.resumeSimulation();
			require(smoke::station::advanceSeconds(baseline, 30) && smoke::station::advanceSeconds(labelled, 30), "reference chemistry run failed");
			auto snapshot = labelled.getStationSystemsSnapshot();
			for (auto& cell : snapshot.fuelCells)
			{
				require(cell.chemistry == core::FuelCellChemistryNames.at(static_cast<size_t>(chemistry)),
					"reference snapshot chemistry missing");
				cell.chemistry = "PEM";
			}
			require(snapshot == baseline.getStationSystemsSnapshot(), "chemistry changed reference simulation");
		}
	}

	// ------------------------------------------------------------------
	// The documented reproducible normal-operation scenario: one
	// simulated hour with the accounting summaries closed and every
	// demonstrated stage running.
	// ------------------------------------------------------------------
	void referenceNominal(smoke::Context const& context)
	{
		auto const& reference = core::bundledStationReference();
		auto const& rig = reference.settings;
		core::World world("ReferenceNominal", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		configureOrDiagnose(world, rig, "reference nominal");
		world.resumeSimulation();
		auto const total = scenarioTicks(reference.scenarioSeconds);
		// Run in quarter-hour legs so an intermediate failure names itself.
		runOrDiagnose(world, total / 4, "reference nominal leg 1");
		runOrDiagnose(world, total / 4, "reference nominal leg 2");
		runOrDiagnose(world, total / 4, "reference nominal leg 3");
		runOrDiagnose(world, total - 3 * (total / 4), "reference nominal leg 4");
		auto const& snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "the reference nominal run was not healthy");
		for (auto const ticks : snap.completedTicks)
			require(ticks == smoke::station::expectedStationSteps(world), "a station system fell behind in the reference nominal run");

		// The crew runs at full scale: the human source of the whole chain.
		auto const& crew = firstOf(snap.crews);
		require(crew.crewCount == 4 && crew.foodConsumed > 0.0
			&& crew.co2ProducedKg > 0.0 && crew.urineProduced > 0.0,
			"the reference crew did not metabolise");
		require(crew.metabolicScale >= 1.0 - 1e-9,
			"the reference crew ran throttled in nominal operation");

		// Habitat quality holds: oxygen above its floor, CO2 below its
		// ceiling, no limit crossings.
		auto const& hab = atmosSnap(snap, "hab");
		require(!hab.lowOxygen && !hab.highCarbonDioxide
			&& !hab.belowPressureLimit && !hab.abovePressureLimit,
			"the reference hab crossed a declared quality or pressure limit in nominal operation");
		require(hab.electrolysisOxygenKg > 0.0 && hab.co2CapturedKg > 0.0,
			"the hab did not exchange with the oxygen makeup and CO2 capture services");

		// Water makeup and recovery: the collector condenses cabin
		// humidity, the treatment upgrades raw to fresh.
		require(!snap.waterCollectors.empty() && firstOf(snap.waterCollectors).collectedKg > 0.0,
			"the hab water collector condensed nothing in nominal operation");
		auto const& wtr = std::find_if(snap.waterTreatments.begin(), snap.waterTreatments.end(),
			[](core::StationWaterTreatmentSnapshot const& t) { return t.id == "wtr"; })
			!= snap.waterTreatments.end()
			? *std::find_if(snap.waterTreatments.begin(), snap.waterTreatments.end(),
				[](core::StationWaterTreatmentSnapshot const& t) { return t.id == "wtr"; })
			: core::StationWaterTreatmentSnapshot{};
		require(wtr.processedKg > 0.0 && wtr.treatedKg > 0.0,
			"the normal water treatment processed nothing in nominal operation");

		// Hydrogen/carbon balance: electrolysis feeds the Sabatier stage,
		// pyrolysis recycles hydrogen back, and the loop retains a net
		// hydrogen demand - it never reads as self-sustaining.
		auto const& lyzer = firstOf(snap.electrolyzers);
		auto const& sab = firstOf(snap.sabatierReactors);
		auto const& pyro = firstOf(snap.methaneProcessors);
		require(lyzer.hydrogenProducedKg > 0.0, "the electrolyzer produced no hydrogen");
		require(sab.co2ConsumedKg > 0.0 && sab.methaneProducedKg > 0.0,
			"the Sabatier reactor did not run in the reference nominal scenario");
		require(pyro.methaneConsumedKg > 0.0 && pyro.hydrogenReturnedKg > 0.0,
			"the methane processor did not recycle hydrogen in the reference nominal scenario");
		require(sab.hydrogenConsumedKg > pyro.hydrogenReturnedKg,
			"the carbon loop reads as hydrogen-self-sustaining - the reconciled net demand vanished");
		auto const& h2 = storeSnap(snap, "hydrogen");
		auto const netDemand = sab.hydrogenConsumedKg - pyro.hydrogenReturnedKg;
		require(std::abs((h2.currentQualified - h2.startingQualified)
			- (lyzer.hydrogenProducedKg - netDemand)) <= 1e-5 * lyzer.hydrogenProducedKg,
			"the hydrogen store did not close over production minus the net carbon-loop demand");

		// Microbial participation: the digester ate feed, recovered
		// nutrients and water, and the fertPump fed the crop nitrogen
		// store.
		auto const& dig = firstOf(snap.microbialProcessors);
		require(dig.feedConsumedKg > 0.0 && dig.nutrientProducedKg > 0.0
			&& dig.recoveredWaterKg > 0.0,
			"the microbial digester did not participate in nominal operation");
		require(storeSnap(snap, "fertN").currentQualified > 0.0,
			"the recovered nutrient pump never reached the crop nitrogen store");

		// Biology grew and harvested: the crew's food pathway closes.
		auto const& crop = firstOf(snap.cropPopulations);
		auto const& algae = firstOf(snap.algaeCultures);
		require(crop.growthKg > 0.0 && algae.growthKg > 0.0,
			"the reference biology did not grow in nominal operation");
		require(crop.harvestedKg > 0.0 && algae.harvestedKg > 0.0,
			"the reference harvest contract did not fire in the nominal scenario");
		require(storeSnap(snap, "food").currentQualified > 0.0,
			"the food store ended the nominal scenario empty");

		// Power: the solar array carried the station; the battery was not
		// the sustained source and the backup stayed commanded off.
		require(firstOf(snap.power.sources).availableKW > 0.0, "the reference solar array generated nothing");
		require(snap.power.unmetCriticalKW <= 1e-9,
			"the reference nominal run left unmet critical electrical demand");
		require(firstOf(snap.power.generators).phase == core::StationGeneratorPhase::Stopped
			&& firstOf(snap.fuelCells).commandedOutputKW == 0.0,
			"a backup source ran in the reference nominal scenario");
		require(findLoad(snap, "lifeSupport").suppliedKW >= 1.5 - 1e-9
			&& findLoad(snap, "comms").suppliedKW >= 0.5 - 1e-9,
			"a protected load was not fully supplied in nominal operation");

		// Thermal: the chilled service ran and the process sink stayed
		// inside its declared limits.
		require(firstOf(snap.chillers).extractedEnergyKJ > 0.0,
			"the chilled service extracted nothing in nominal operation");
		auto const& sink = std::find_if(snap.thermal.reservoirs.begin(), snap.thermal.reservoirs.end(),
			[](core::StationThermalReservoirSnapshot const& r) { return r.id == "processSink"; })
			!= snap.thermal.reservoirs.end()
			? *std::find_if(snap.thermal.reservoirs.begin(), snap.thermal.reservoirs.end(),
				[](core::StationThermalReservoirSnapshot const& r) { return r.id == "processSink"; })
			: core::StationThermalReservoirSnapshot{};
		auto const sinkLimits = std::find_if(rig.thermalReservoirs.begin(), rig.thermalReservoirs.end(),
			[](core::ThermalReservoirSettings const& r) { return r.id == "processSink"; });
		require(sinkLimits != rig.thermalReservoirs.end(), "reference process sink missing");
		require(sink.temperatureC > sinkLimits->minTemperatureC
			&& sink.temperatureC < sinkLimits->maxTemperatureC,
			"the process sink left its declared limits in nominal operation");

		// Scheduled scenario machinery fired: the mid-scenario resupply
		// arrived, the outage window opened and closed, and the
		// maintenance job completed its first occurrence.
		require(findDelivery(snap, "midFoodResupply").occurrencesCompleted == 1,
			"the scheduled mid-scenario resupply did not arrive");
		require(findOutage(snap, "genOverhaul").occurrencesCompleted == 1,
			"the scheduled generator outage did not complete");
		require(findMaintenance(snap, "maintChiller").occurrencesCompleted == 1,
			"the scheduled maintenance job did not complete its first occurrence");
		require(storeSnap(snap, "maintenanceWaste").currentQualified > 0.0,
			"the maintenance job consumed nothing into its waste store");

		// Closed accounting: every per-unit boundary residual stays inside
		// the published tolerance.
		for (auto const& balance : snap.balances)
			require(std::abs(balance.residual) <= snap.balanceTolerance
				* std::max(1.0, balance.totalStarting),
				("the " + balance.unit + " boundary balance did not close in the reference nominal run").c_str());
		for (auto const& balance : snap.constituentBalances)
			require(std::abs(balance.residual) <= snap.balanceTolerance
				* std::max(1.0, balance.starting),
				("the " + balance.constituent + " constituent balance did not close").c_str());

		// Determinism: reset replays the scenario tick-for-tick.
		auto const snapshotAtEnd = snap;
		auto const eventsAtEnd = world.getStationSystemsEvents();
		world.pauseSimulation();
		world.resetSimulation();
		world.resumeSimulation();
		runOrDiagnose(world, total, "reference nominal replay");
		require(world.getStationSystemsSnapshot() == snapshotAtEnd,
			"the reference reset replay produced a different snapshot");
		require(world.getStationSystemsEvents() == eventsAtEnd,
			"the reference reset replay produced a different event history");

		// Document round trip: the reference is authored settings only,
		// and the reopened World reproduces the same run.
		auto const path = context.temporaryRoot() / "reference-nominal.world.yaml";
		world.pauseSimulation();
		world.saveTo(path.string());
		auto const reopened = core::loadWorldDocument(path);
		require(reopened != nullptr, "the reference document failed to reopen");
		require(reopened->getStationSystemsSettings() == rig,
			"the reference configuration did not survive the document round trip");
		reopened->resumeSimulation();
		runOrDiagnose(*reopened, total, "reference reopened run");
		require(reopened->getStationSystemsSnapshot() == snapshotAtEnd,
			"the reopened reference World did not reproduce the run");
		require(reopened->getStationSystemsEvents() == eventsAtEnd,
			"the reopened reference World produced a different event history");
	}

	// ------------------------------------------------------------------
	// Acceptance of intentional operational shortages and refusal of
	// physically inconsistent inputs.
	// ------------------------------------------------------------------
	void referenceShortageAndRejection(smoke::Context const&)
	{
		auto const& reference = core::bundledStationReference();

		// An intentional operational shortage: strip the potable resupply
		// and start the drinking store thin. The configuration stays
		// physically consistent, so it is accepted, and the endurance
		// explanation reports the thinning drawable supply instead of
		// the station being unbuildable.
		{
			auto thin = reference.settings;
			thin.deliveries.erase(std::remove_if(thin.deliveries.begin(), thin.deliveries.end(),
				[](core::ResourceDeliverySettings const& delivery)
				{ return delivery.storeId == "drinking"; }), thin.deliveries.end());
			for (auto& store : thin.stores)
				if (store.id == "drinking")
				{
					store.startingQualified = 60.0;
					store.protectedReserve = 50.0;
				}
			std::string diagnostic;
			require(core::validateStationSystemsSettings(thin, diagnostic),
				("an intentional potable shortage was refused: " + diagnostic).c_str());
			core::World world("ReferenceShortage", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			configureOrDiagnose(world, thin, "reference shortage");
			world.resumeSimulation();
			runOrDiagnose(world, scenarioTicks(1800.0), "reference shortage run");
			auto const& snap = world.getStationSystemsSnapshot();
			require(snap.healthy, "the intentional shortage run was not healthy");
			// The crew still drew its water: the shortage is a countdown,
			// not a refusal to operate.
			require(storeSnap(snap, "drinking").currentQualified < 60.0,
				"the crew drew nothing from the thin potable store");
			bool drinkingEndurance = false;
			for (auto const& endurance : snap.endurance)
				if (endurance.id == "drinking" && endurance.basis
					== core::StationEnduranceBasis::Depleting) drinkingEndurance = true;
			require(drinkingEndurance,
				"the thin potable store was not explained as a depleting drawable supply");
		}

		// A physically inconsistent edit is refused at the seam with a
		// diagnostic and no mutation: starting inventory above capacity.
		{
			auto broken = reference.settings;
			for (auto& store : broken.stores)
				if (store.id == "food") store.startingQualified = store.capacity + 1.0;
			std::string diagnostic;
			require(!core::validateStationSystemsSettings(broken, diagnostic),
				"a store starting above its capacity was accepted");
			core::World world("ReferenceBroken", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::string refused;
			require(!world.configureStationSystems(broken, false, &refused),
				"the World accepted a store starting above capacity");
			require(!refused.empty(), "the refusal carried no diagnostic");
			require(!world.getStationSystemsSnapshot().enabled,
				"the refused configuration still enabled the station");
		}

		// A crew declaration that would create mass is refused too: the
		// reference's element closure is not a suggestion.
		{
			auto broken = reference.settings;
			for (auto& crew : broken.crews) crew.co2PerCrewKgPerS = 1.0;
			std::string diagnostic;
			require(!core::validateStationSystemsSettings(broken, diagnostic),
				"a crew that creates mass was accepted");
		}
	}
}

void registerReferenceStationChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"referenceBundled", referenceBundled});
	checks.push_back({"referenceNominal", referenceNominal});
	checks.push_back({"referenceShortageAndRejection", referenceShortageAndRejection});
}
