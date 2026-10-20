// #421: coupled microbial and carbon-loop operation.
//
// These checks drive the coupled rig (CoupledRig.h) - one World where the
// crew, the CO2 capture chain, the Sabatier/methane-processing carbon
// loop, the microbial nutrient loop, the crop/algae biology, the water
// chain, the power pool and the waste boundary all share the same stores,
// compartments and clock - through the public World tick/snapshot/event
// seam. They assert that a change in one stage propagates into the
// dependent resources and services, that the limiting process is named in
// the snapshot and announced in the event log, that normal operation
// closes its accounting, and that the coupled run is deterministic across
// reset and document round trips.
#include "Coupled.h"
#include "../CoupledRig.h"
#include "core/World.h"
#include "core/AgentTagRegistryDocument.h"

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

	auto configure = [](core::World& world, core::StationSystemsSettings const& settings,
		char const* what)
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
		if (world.advanceTicks(ticks)) return;
		auto const diagnostics = world.getStationSystemsDiagnostics();
		std::fprintf(stderr, "%s run failed: %s\n", what,
			diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
		throw std::runtime_error(std::string(what) + " run failed");
	};

	auto sawEvent = [](core::World& world, std::string const& needle)
	{
		for (auto const& event : world.getStationSystemsEvents())
			if (event.message.find(needle) != std::string::npos) return true;
		return false;
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
	auto crewSnap = [](core::StationSystemsSnapshot const& snap)
	{
		return snap.crews.empty() ? core::StationCrewSnapshot{} : snap.crews.front();
	};
	auto sabatierSnap = [](core::StationSystemsSnapshot const& snap)
	{
		return snap.sabatierReactors.empty() ? core::StationSabatierReactorSnapshot{}
			: snap.sabatierReactors.front();
	};
	auto pyroSnap = [](core::StationSystemsSnapshot const& snap)
	{
		return snap.methaneProcessors.empty() ? core::StationMethaneProcessorSnapshot{}
			: snap.methaneProcessors.front();
	};
	auto digSnap = [](core::StationSystemsSnapshot const& snap)
	{
		return snap.microbialProcessors.empty() ? core::StationMicrobialProcessorSnapshot{}
			: snap.microbialProcessors.front();
	};
	auto cropSnap = [](core::StationSystemsSnapshot const& snap)
	{
		return snap.cropPopulations.empty() ? core::StationCropPopulationSnapshot{}
			: snap.cropPopulations.front();
	};
	auto algaeSnap = [](core::StationSystemsSnapshot const& snap)
	{
		return snap.algaeCultures.empty() ? core::StationAlgaeCultureSnapshot{}
			: snap.algaeCultures.front();
	};
	auto captureSnap = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.co2Captures)
			if (entry.id == id) return entry;
		return core::StationCo2CaptureSnapshot{};
	};
	auto transferSnap = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.transfers)
			if (entry.id == id) return entry;
		return core::StationTransferSnapshot{};
	};
	auto ejectionSnap = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.wasteEjections)
			if (entry.id == id) return entry;
		return core::StationWasteEjectionSnapshot{};
	};

	// A coupled World brought up from the given rig settings and resumed.
	auto startCoupled = [](core::World& world, core::StationSystemsSettings const& settings,
		char const* what)
	{
		world.pauseSimulation();
		world.finishBuild();
		configure(world, settings, what);
		world.resumeSimulation();
	};

	// ------------------------------------------------------------------
	// Nominal coupled operation: every loop runs together, the accounting
	// closes, and the whole coupled picture is deterministic.
	// ------------------------------------------------------------------
	void coupledNominal(smoke::Context const& context)
	{
		auto const h2PerCo2 = core::StationSabatierHydrogenMassPerCarbonDioxideKg;
		auto const ch4PerCo2 = core::StationSabatierMethaneMassPerCarbonDioxideKg;
		auto const h2PerCh4 = core::StationMethaneProcessingHydrogenMassPerMethaneKg;
		auto const rig = coupled_rig::nominal();
		std::string diagnostic;
		require(core::validateStationSystemsSettings(rig, diagnostic),
			("the nominal coupled rig was refused: " + diagnostic).c_str());

		core::World world("CoupledNominal", 2, 2);
		startCoupled(world, rig, "coupled nominal");
		runOrDiagnose(world, 7200, "coupled nominal run"); // 120 s
		auto const& snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "the coupled run was not healthy");
		for (auto const ticks : snap.completedTicks)
			require(ticks == 7200, "a station system fell behind in the coupled run");

		// The crew runs and produces: the human CO2 and waste source.
		auto const& crew = crewSnap(snap);
		require(crew.crewCount == 2 && crew.co2ProducedKg > 0.0 && crew.foodConsumed > 0.0
			&& crew.organicWasteProduced > 0.0,
			"the coupled crew did not metabolize");

		// Crew CO2 is captured out of the hab into the shared carbon store,
		// and the hab keeps breathing (electrolyzer O2 delivery).
		auto const& hab = atmosSnap(snap, "hab");
		require(hab.co2CapturedKg > 0.0 && hab.oxygenConsumedKg > 0.0
			&& hab.electrolysisOxygenKg > 0.0,
			"the hab atmosphere did not exchange with the station");
		require(!hab.lowOxygen, "the hab went low on oxygen in nominal operation");

		// Non-human CO2 sources feed the same loop: crop respiration CO2
		// is captured from the greenhouse, and the digester books its own
		// CO2 product into the shared store.
		require(cropSnap(snap).respirationCo2ReturnedKg > 0.0,
			"the crops respired no CO2 into the coupled loop");
		require(atmosSnap(snap, "greenhouse").co2CapturedKg > 0.0,
			"crop-respiration CO2 never reached the capture store");
		require(digSnap(snap).co2ProducedKg > 0.0,
			"the digester produced no CO2 for the coupled loop");

		// The carbon loop converts and closes on its declared stoichiometry.
		auto const& sab = sabatierSnap(snap);
		require(sab.co2ConsumedKg > 0.0 && sab.methaneProducedKg > 0.0
			&& sab.waterProducedKg > 0.0,
			"the Sabatier reactor did not run in the coupled loop");
		require(closeTo(sab.hydrogenConsumedKg, sab.co2ConsumedKg * h2PerCo2),
			"the Sabatier hydrogen draw does not match the CO2 converted");
		require(closeTo(sab.methaneProducedKg, sab.co2ConsumedKg * ch4PerCo2),
			"the Sabatier methane output does not match the CO2 converted");
		auto const& pyro = pyroSnap(snap);
		require(pyro.methaneConsumedKg > 0.0 && pyro.hydrogenReturnedKg > 0.0
			&& pyro.carbonProducedKg > 0.0,
			"the methane processor did not run in the coupled loop");
		require(closeTo(pyro.hydrogenReturnedKg, pyro.methaneConsumedKg * h2PerCh4),
			"the recycled hydrogen does not match the methane processed");

		// Net Sabatier hydrogen demand without duplicate accounting: the
		// recycled return is counted once as store inflow, the reactor's
		// draw once, and the h2 store's movement closes over exactly
		// electrolyzer production minus the net demand the recycle left.
		auto const netHydrogenDemand = sab.hydrogenConsumedKg - pyro.hydrogenReturnedKg;
		require(netHydrogenDemand > 0.0,
			"the methane recycle covered the Sabatier hydrogen demand - the loop reads as self-sustaining");
		auto const& h2 = storeSnap(snap, "h2");
		auto const lyzerH2 = snap.electrolyzers.empty() ? 0.0
			: snap.electrolyzers.front().hydrogenProducedKg;
		auto const h2ClosureTolerance = 1e-6 * (lyzerH2 + sab.hydrogenConsumedKg
			+ pyro.hydrogenReturnedKg);
		require(std::abs((h2.currentQualified - h2.startingQualified)
			- (lyzerH2 - netHydrogenDemand)) <= h2ClosureTolerance,
			"the hydrogen store did not close over production minus the net Sabatier demand "
			"(duplicate accounting of the recycled hydrogen?)");

		// The microbial nutrient loop feeds the crops: recovered nutrients
		// are pumped into the crop nitrogen store and actually taken up.
		require(digSnap(snap).nutrientProducedKg > 0.0,
			"the digester recovered no nutrients");
		require(transferSnap(snap, "fertPump").lastCompleted > 0.0,
			"the nutrient-to-fertilizer pump moved nothing this tick");
		require(cropSnap(snap).nitrogenDrawnKg > 0.0,
			"the crops drew no nutrient nitrogen from the loop");
		require(cropSnap(snap).growthKg > 0.0 && algaeSnap(snap).growthKg > 0.0,
			"the coupled biology did not grow");

		// The harvest feeds back into the crew's food store; the food
		// store's movement closes over harvest inflow minus crew draw.
		require(cropSnap(snap).harvestedKg > 0.0 && algaeSnap(snap).harvestedKg > 0.0,
			"no harvest happened in the nominal coupled run");
		auto const& food = storeSnap(snap, "food");
		auto const harvestIn = cropSnap(snap).foodProducedKg + algaeSnap(snap).foodProducedKg;
		auto const foodClosureTolerance = 1e-6 * (harvestIn + crew.foodConsumed);
		require(std::abs((food.currentQualified - food.startingQualified)
			- (harvestIn - crew.foodConsumed)) <= foodClosureTolerance,
			"the food store did not close over harvest inflow minus crew consumption");

		// The terminal boundary moves the solid carbon the loop makes.
		require(ejectionSnap(snap, "ejectCarbon").ejectedTotal > 0.0,
			"the solid carbon from methane processing was never ejected");

		// Closed accounting: the per-unit boundary residual for kg stays
		// within the published tolerance.
		for (auto const& balance : snap.balances)
			require(std::abs(balance.residual) <= snap.balanceTolerance
				* std::max(1.0, balance.totalStarting),
				("the " + balance.unit + " boundary balance did not close").c_str());

		// Determinism: reset replays the coupled run tick-for-tick.
		auto const snapshotAt120 = snap;
		auto const eventsAt120 = world.getStationSystemsEvents();
		world.pauseSimulation();
		world.resetSimulation();
		world.resumeSimulation();
		runOrDiagnose(world, 7200, "coupled nominal replay");
		require(world.getStationSystemsSnapshot() == snapshotAt120,
			"the coupled reset replay produced a different snapshot");
		require(world.getStationSystemsEvents() == eventsAt120,
			"the coupled reset replay produced a different event history");

		// Document round trip: the coupled scenario is authored settings
		// only, and the reopened World reproduces the same run.
		auto const path = context.temporaryRoot() / "coupled-nominal.world.yaml";
		world.pauseSimulation();
		world.saveTo(path.string());
		auto const reopened = core::loadWorldDocument(path);
		require(reopened != nullptr, "the coupled document failed to reopen");
		require(reopened->getStationSystemsSettings() == rig,
			"the coupled rig did not survive the document round trip");
		reopened->resumeSimulation();
		runOrDiagnose(*reopened, 7200, "coupled reopened run");
		require(reopened->getStationSystemsSnapshot() == snapshotAt120,
			"the reopened coupled World did not reproduce the run");
		require(reopened->getStationSystemsEvents() == eventsAt120,
			"the reopened coupled World produced a different event history");
	}

	// ------------------------------------------------------------------
	// Stress: a broken stage propagates through the dependent resources
	// and services, names its limiter, and recovers when the constraint
	// is lifted.
	// ------------------------------------------------------------------
	void coupledStress(smoke::Context const&)
	{
		// Scenario A: digester culture-medium exhaustion stops nutrient
		// recovery; the crop nitrogen store drains; the canopy goes
		// nitrogen-limited; a medium delivery restarts the loop.
		{
			auto rig = coupled_rig::nominal();
			for (auto& store : rig.stores)
			{
				if (store.id == "digesterMedia") store.startingQualified = 0.0005;
				if (store.id == "nutrients") store.startingQualified = 0.0;
				if (store.id == "fertN") store.startingQualified = 0.002;
			}
			rig.microbialProcessors.front().rateKgPerS = 0.002;
			core::ResourceDeliverySettings mediaDrop;
			mediaDrop.id = "mediaDrop";
			mediaDrop.storeId = "digesterMedia";
			mediaDrop.unit = "kg";
			mediaDrop.quantity = 1.0;
			mediaDrop.firstOccurrenceSeconds = 120.0;
			rig.deliveries.push_back(mediaDrop);

			core::World world("CoupledLoss", 2, 2);
			startCoupled(world, rig, "coupled loss");
			runOrDiagnose(world, 3600, "coupled loss phase 1"); // 60 s
			auto const& snap = world.getStationSystemsSnapshot();
			require(snap.healthy, "the coupled loss run was not healthy");
			auto const& dig = digSnap(snap);
			require(dig.status == core::StationTransferStatus::Rejected
				&& dig.blockReason.find("consumable store 'digesterMedia'")
					!= std::string::npos,
				"the digester did not stop on culture-medium exhaustion");
			require(dig.lastStressors.find("consumable") != std::string::npos,
				"the digester stressor report missed the medium exhaustion");
			require(dig.lastNutrientProducedKg == 0.0,
				"the starved digester kept producing nutrients");
			auto const& crop = cropSnap(snap);
			require(crop.lastGrowthScale <= 1e-12
				&& crop.lastGrowthLimiter.find("nitrogen store 'fertN'") != std::string::npos,
				"the canopy did not go nitrogen-limited when the nutrient supply stopped");
			require(sawEvent(world, "nitrogen store 'fertN'"),
				"the nitrogen limitation was never announced in the event log");

			// Lift the constraint: the medium delivery restarts recovery,
			// the pump refills the crop nitrogen store and growth resumes.
			runOrDiagnose(world, 3600, "coupled recovery phase"); // to 120 s
			runOrDiagnose(world, 3600, "coupled recovery phase 2"); // to 180 s
			auto const& later = world.getStationSystemsSnapshot();
			require(later.healthy, "the coupled recovery run was not healthy");
			require(digSnap(later).lastFeedConsumedKg > 0.0
				&& digSnap(later).lastNutrientProducedKg > 0.0,
				"the digester did not resume after the medium delivery");
			require(digSnap(later).recovering,
				"the digester did not report recovery after the medium delivery");
			require(storeSnap(later, "fertN").currentQualified > 0.0,
				"the crop nitrogen store was never refilled after recovery");
			require(cropSnap(later).lastGrowthScale > 0.0,
				"the canopy did not resume growth after the nutrient loop recovered");
		}

		// Scenario B1: no hydrogen - the carbon stage stops at the
		// hydrogen feed and the CO2 inventory piles up behind it.
		{
			auto rig = coupled_rig::nominal();
			for (auto& store : rig.stores)
				if (store.id == "h2") store.startingQualified = 0.0;
			rig.electrolyzers.front().rateKgPerS = 0.0;
			core::World world("CoupledH2Starved", 2, 2);
			startCoupled(world, rig, "coupled h2 starved");
			runOrDiagnose(world, 600, "coupled h2 starved run");
			auto const& snap = world.getStationSystemsSnapshot();
			require(snap.healthy, "the h2-starved coupled run was not healthy");
			require(sabatierSnap(snap).status == core::StationTransferStatus::Rejected
				&& sabatierSnap(snap).blockReason.find("hydrogen feed store 'h2'")
					!= std::string::npos,
				"the Sabatier reactor did not stop on the empty hydrogen store");
			require(storeSnap(snap, "co2").currentQualified
				> storeSnap(snap, "co2").startingQualified,
				"CO2 did not accumulate behind the stopped carbon conversion");
			require(sawEvent(world, "Sabatier reactor 'sab' rejected: hydrogen feed store"),
				"the hydrogen starvation was never announced in the event log");
		}

		// Scenario B2: the methane product destination is full - the
		// reactor throttles, the capture store backs up, the capture
		// service fails, and the hab CO2 limit is crossed.
		{
			auto rig = coupled_rig::nominal();
			for (auto& store : rig.stores)
			{
				if (store.id == "co2") { store.capacity = 5.2; store.startingQualified = 5.0; }
				if (store.id == "methane") { store.capacity = 0.05; store.startingQualified = 0.0; }
			}
			rig.methaneProcessors.front().rateKgPerS = 0.0;
			core::World world("CoupledMethaneFull", 2, 2);
			startCoupled(world, rig, "coupled methane full");
			// Run past the capture-store-full transition (~133 s) and check
			// the announcement while it is still inside the bounded 256-entry
			// event history, before continuing to the long-haul assertions.
			runOrDiagnose(world, 10200, "coupled methane full early run"); // 170 s
			require(sawEvent(world, "capture store 'co2' is full"),
				"the capture backpressure was never announced in the event log");
			runOrDiagnose(world, 25800, "coupled methane full run"); // 600 s total
			auto const& snap = world.getStationSystemsSnapshot();
			require(snap.healthy, "the methane-full coupled run was not healthy");
			require(sabatierSnap(snap).blockReason.find("methane store 'methane' is full")
				!= std::string::npos,
				"the Sabatier reactor did not report the full methane destination");
			require(captureSnap(snap, "scrubHab").limiter
				.find("capture store 'co2' is full") != std::string::npos,
				"the hab CO2 capture did not report the full capture store");
			require(atmosSnap(snap, "hab").highCarbonDioxide,
				"the hab CO2 quality limit was never crossed behind the backed-up capture chain");
		}

		// Scenario B3: the digester residue destination is full - the
		// whole digestion stage backpressures.
		{
			auto rig = coupled_rig::nominal();
			for (auto& store : rig.stores)
				if (store.id == "residue") store.capacity = 0.05;
			rig.microbialProcessors.front().rateKgPerS = 0.002;
			core::World world("CoupledResidueFull", 2, 2);
			startCoupled(world, rig, "coupled residue full");
			runOrDiagnose(world, 3600, "coupled residue full run");
			auto const& snap = world.getStationSystemsSnapshot();
			require(snap.healthy, "the residue-full coupled run was not healthy");
			require(digSnap(snap).blockReason.find("residue store 'residue' is full")
				!= std::string::npos,
				"the digester did not report the full residue destination");
			require(digSnap(snap).lastFeedConsumedKg == 0.0,
				"the backpressured digester kept consuming feed it could not place");
		}

		// Scenario C: reduced carbon recovery propagates - with the
		// Sabatier stage disabled the CO2 the loop would have converted
		// stays in the capture store, the product water never appears,
		// and no methane is made.
		{
			auto const nominalRig = coupled_rig::nominal();
			core::World nominal("CoupledCBase", 2, 2);
			startCoupled(nominal, nominalRig, "coupled C base");
			runOrDiagnose(nominal, 7200, "coupled C base run");

			auto reduced = coupled_rig::nominal();
			reduced.sabatierReactors.front().rateKgPerS = 0.0;
			core::World reducedWorld("CoupledCReduced", 2, 2);
			startCoupled(reducedWorld, reduced, "coupled C reduced");
			runOrDiagnose(reducedWorld, 7200, "coupled C reduced run");

			auto const& base = nominal.getStationSystemsSnapshot();
			auto const& cut = reducedWorld.getStationSystemsSnapshot();
			require(cut.healthy && base.healthy, "a reduced-recovery run was not healthy");
			require(sabatierSnap(cut).methaneProducedKg == 0.0
				&& sabatierSnap(base).methaneProducedKg > 0.0,
				"the Sabatier production does not distinguish the disabled carbon stage");
			require(storeSnap(cut, "co2").currentQualified
				> storeSnap(base, "co2").currentQualified,
				"disabling carbon recovery did not leave more CO2 in the shared store");
			require(storeSnap(cut, "rawWater").currentQualified
				< storeSnap(base, "rawWater").currentQualified,
				"disabling carbon recovery did not reduce the product water reaching treatment");
		}

		// Scenario E: an eclipse cuts the power pool - the electrical
		// shortfall propagates into the process stages and the biology,
		// and the battery rides the transient.
		{
			auto rig = coupled_rig::nominal();
			core::ProfileSettings eclipse;
			eclipse.periodSeconds = 120.0;
			eclipse.segments = {{0.0, 1.0}, {60.0, 0.0}};
			rig.solarArrays.front().illumination = eclipse;
			core::World world("CoupledEclipse", 2, 2);
			startCoupled(world, rig, "coupled eclipse");
			// Process-stage shortfall is reported per process, not in the
			// equipment-load pool totals; sum the coupled stages.
			auto const processUnmet = [](core::StationSystemsSnapshot const& s)
			{
				double total = 0.0;
				for (auto const& c : s.co2Captures) total += c.lastPowerUnmetKW;
				for (auto const& c : s.electrolyzers) total += c.lastPowerUnmetKW;
				for (auto const& r : s.sabatierReactors) total += r.lastPowerUnmetKW;
				for (auto const& p : s.methaneProcessors) total += p.lastPowerUnmetKW;
				for (auto const& t : s.waterTreatments) total += t.lastPowerUnmetKW;
				for (auto const& p : s.microbialProcessors) total += p.lastPowerUnmetKW;
				for (auto const& c : s.cropPopulations)
					total += std::max(0.0, c.lastLightRequestedKW - c.lastLightSuppliedKW);
				for (auto const& c : s.algaeCultures)
					total += std::max(0.0, c.lastLightRequestedKW - c.lastLightSuppliedKW);
				return total;
			};
			runOrDiagnose(world, 3540, "coupled eclipse run"); // 59 s: full sun
			auto const& sun = world.getStationSystemsSnapshot();
			require(sun.healthy && processUnmet(sun) <= 1e-9,
				"the coupled rig showed process power shortfall in full sun");
			runOrDiagnose(world, 3600, "coupled eclipse dark run"); // to 119 s
			auto const& dark = world.getStationSystemsSnapshot();
			require(dark.healthy, "the eclipse run was not healthy");
			require(processUnmet(dark) > 0.0,
				"the eclipse produced no electrical shortfall against the coupled demand");
			require(dark.power.batteryDischargeKW > 0.0,
				"the battery did not ride the eclipse transient");
			require(cropSnap(dark).lastLightSuppliedKW < cropSnap(dark).lastLightRequestedKW
				|| cropSnap(dark).lastGrowthScale < 1.0,
				"the crop lamps were unaffected by the power shortfall");
		}
	}

}

void registerCoupledStationChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"coupledNominal", coupledNominal});
	checks.push_back({"coupledStress", coupledStress});
}
