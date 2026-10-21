#include "StationReferenceFixtures.h"
#include "StationCoreChecks.h"
#include "core/StationProtection.h"
#include "core/StationReference.h"
#include "core/StationSystemsRuntime.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
	constexpr float quantum = 1.0f / 60.0f;
	void require(bool ok, std::string const& message) { if (!ok) throw std::runtime_error(message); }
	enum Kind { Generator, FuelCell, Electrolyzer, Sabatier, Methane, Microbial, Crop, Algae, Chiller };
	constexpr char const* names[] = {"Generator", "FuelCell", "Electrolyzer", "Sabatier", "Methane", "Microbial", "Crop", "Algae", "Chiller"};

	core::StationSystemsSettings fixture(Kind kind, uint32_t ratio, bool tight, bool cold = false)
	{
		auto s = core::bundledStationReference().settings;
		s.systemsStepWorldTicks = ratio;
		s.dispatchMode = core::StationDispatchMode::Manual;
		s.transfers.clear(); s.conversions.clear(); s.deliveries.clear(); s.wasteEjections.clear();
		s.maintenanceJobs.clear(); s.outages.clear(); s.crews.clear(); s.airlockCycles.clear();
		s.equipmentLoads.clear(); s.batteries.clear(); s.radiators.clear(); s.thermalBuffers.clear();
		s.waterCollectors.clear(); s.waterTreatments.clear(); s.co2Captures.clear(); s.connections.clear();
		if (kind != Generator) s.generators.clear();
		if (kind != FuelCell) s.fuelCells.clear();
		if (kind != Electrolyzer) s.electrolyzers.clear();
		if (kind != Sabatier) s.sabatierReactors.clear();
		if (kind != Methane) s.methaneProcessors.clear();
		if (kind != Microbial) s.microbialProcessors.clear();
		if (kind != Crop) s.cropPopulations.clear();
		if (kind != Algae) s.algaeCultures.clear();
		if (kind != Chiller) s.chillers.clear();
		for (auto& store : s.stores)
		{
			store.startingQualified = store.capacity / 4;
			store.startingUnqualified = 0;
			store.protectedReserve = 0;
		}
		for (auto& r : s.thermalReservoirs)
		{
			r.heatCapacityKJPerK = 1; r.startingTemperatureC = 25;
			r.minTemperatureC = -50; r.maxTemperatureC = 200; r.absorbedHeatKW = 0;
		}
		for (auto& a : s.atmospheres)
		{
			a.thermalCapacityKJPerK = 1; a.startingTemperatureC = 25;
			a.occupants = 0; a.leakRateKgPerS = 0; a.exchangerConductanceKWPerK = 0;
			a.startingWaterVapourKg = 0;
		}
		for (auto& loop : s.coolantLoops)
		{
			loop.startingTemperatureC = 30; loop.exchangerConductanceKWPerK = 0;
			loop.freezeProtectionSetpointC = 0; loop.heaterCapacityKW = 0;
		}
		for (auto& solar : s.solarArrays)
		{
			solar.capacityKW = 100; solar.illumination = {};
			solar.illumination.constantLevel = 1;
		}
		auto const maximum = tight ? 25.000001 : 150.0;
		for (auto& g : s.generators)
		{
			g.commandedOutputKW = 1; g.startupSeconds = 0; g.startupPowerKW = 0;
			g.shutdownSeconds = 0; g.shutdownHeatKW = 0; g.maxSinkTemperatureC = maximum;
		}
		for (auto& f : s.fuelCells) { f.commandedOutputKW = 1; f.maxSinkTemperatureC = maximum; }
		for (auto& e : s.electrolyzers) e.maxSinkTemperatureC = maximum;
		for (auto& r : s.sabatierReactors) r.maxSinkTemperatureC = maximum;
		for (auto& m : s.methaneProcessors) m.maxSinkTemperatureC = maximum;
		for (auto& m : s.microbialProcessors)
		{
			m.minOperatingTemperatureC = cold && tight ? 24.999999 : 0;
			m.maxSinkTemperatureC = cold ? 150 : maximum;
		}
		for (auto& c : s.cropPopulations)
		{
			c.minGrowingTemperatureC = cold && tight ? 24.999999 : 0;
			c.maxGrowingTemperatureC = cold ? 150 : maximum;
		}
		for (auto& a : s.algaeCultures)
		{
			a.minGrowingTemperatureC = cold && tight ? 24.999999 : 0;
			a.maxGrowingTemperatureC = cold ? 150 : maximum;
		}
		for (auto& c : s.chillers) c.maxCondensingTemperatureC = maximum;
		if (cold)
		{
			// Force the lower qualification boundary, independently of heat
			// production. The heat-only coupling is real, not a test mutation.
			if (kind == Microbial)
			{
				auto& loop = s.coolantLoops.front();
				loop.reservoirId = s.microbialProcessors.front().heatSinkId;
				loop.startingTemperatureC = 0; loop.exchangerConductanceKWPerK = 100;
			}
			else
			{
				for (auto& r : s.thermalReservoirs) r.startingTemperatureC = 0;
				for (auto& a : s.atmospheres)
				{
					a.reservoirId = s.thermalReservoirs.front().id;
					a.exchangerConductanceKWPerK = 100;
				}
			}
		}
		std::string diagnostic;
		require(core::validateStationSystemsSettings(s, diagnostic), names[kind] + std::string(": ") + diagnostic);
		return s;
	}

	double production(core::StationSystemsSnapshot const& s, Kind kind)
	{
		switch (kind)
		{
		case Generator: return s.power.generators.front().fuelEnergyKJ;
		case FuelCell: return s.fuelCells.front().hydrogenConsumedKg;
		case Electrolyzer: return s.electrolyzers.front().waterSplitKg;
		case Sabatier: return s.sabatierReactors.front().co2ConsumedKg;
		case Methane: return s.methaneProcessors.front().methaneConsumedKg;
		case Microbial: return s.microbialProcessors.front().feedConsumedKg;
		case Crop: return s.cropPopulations.front().growthKg;
		case Algae: return s.algaeCultures.front().growthKg;
		case Chiller: return s.chillers.front().extractedEnergyKJ;
		}
		throw std::runtime_error("Unknown fixture");
	}

	void reallocation(smoke::Context const&)
	{
		core::ScopedStationStepRatioForTesting explicitClock(std::nullopt);
		auto s = fixture(Electrolyzer, 6, true);
		s.coolantLoops.clear();
		auto second = s.electrolyzers.front();
		second.id = "second";
		auto sink = *std::find_if(s.thermalReservoirs.begin(), s.thermalReservoirs.end(),
			[&](auto const& r) { return r.id == second.heatSinkId; });
		sink.id = "secondSink"; second.heatSinkId = sink.id;
		s.thermalReservoirs.push_back(sink); s.electrolyzers.push_back(second);
		// Exactly one machine's dose: refusing the first releases power that
		// can make the second unsafe. A one-pass prediction misses this.
		s.solarArrays.front().capacityKW = second.rateKgPerS * second.specificEnergyKWhPerKgWater * 3600;
		s.deliveries = {{"pulse", s.stores.front().id, s.stores.front().unit, 0.01, 0.005, 0.02}};
		core::StationSystemsRuntime runtime(s, quantum);
		require(runtime.advanceTo(6.0 * quantum), "Reallocation drive failed");
		auto const snap = runtime.snapshot();
		for (auto const& e : snap.electrolyzers)
			require(e.waterSplitKg == 0 && !e.coolingAvailable, "Reallocated power bypassed prediction: " + e.id + " split=" + std::to_string(e.waterSplitKg)
					+ " cooling=" + std::to_string(e.coolingAvailable));
		require(std::abs(snap.deliveries.front().acceptedTotal - 0.05) < 1e-12, "Trial duplicated a scheduled delivery");
		for (size_t i = 0; i < s.stores.size(); ++i)
			require(std::abs(snap.stores[i].currentQualified - s.stores[i].startingQualified - (i == 0 ? 0.05 : 0)) < 1e-10,
				"Trial leaked material consumption");
		for (auto const& r : snap.thermal.reservoirs)
			require(r.temperatureC == 25 && r.residualKJ == 0, "Trial leaked heat");
	}

	void grossExcursion(smoke::Context const&)
	{
		core::ScopedStationStepRatioForTesting explicitClock(std::nullopt);
		auto s = fixture(Electrolyzer, 6, true);
		s.coolantLoops.clear();
		// The net endpoint cools. Protection must nevertheless consider the
		// gross heating excursion without crediting simultaneous rejection.
		s.radiators = {{"dump", s.electrolyzers.front().heatSinkId, 1, 1, 1, -270}};
		core::StationSystemsRuntime runtime(s, quantum);
		require(runtime.advanceTo(6.0 * quantum), "Gross excursion drive failed");
		auto const snap = runtime.snapshot();
		require(snap.electrolyzers.front().waterSplitKg == 0, "Net cooling hid an unsafe gross heating rate");
		auto const& r = *std::find_if(snap.thermal.reservoirs.begin(), snap.thermal.reservoirs.end(),
			[&](auto const& body) { return body.id == s.electrolyzers.front().heatSinkId; });
		require(r.temperatureC < 25 && std::abs(r.residualKJ) < 1e-9, "Refusal clamped or deleted unconditional cooling");
	}

	void freeze(smoke::Context const&)
	{
		core::ScopedStationStepRatioForTesting explicitClock(std::nullopt);
		for (bool shortage : {false, true})
		{
			core::StationSystemsSettings s;
			s.enabled = true; s.systemsStepWorldTicks = 6;
			core::ThermalReservoirSettings r;
			r.id = "cold"; r.thermalClass = "moderate"; r.heatCapacityKJPerK = 1000;
			r.startingTemperatureC = 0; r.minTemperatureC = -10; r.maxTemperatureC = 50;
			s.thermalReservoirs = {r};
			core::CoolantLoopSettings loop;
			loop.id = "loop"; loop.reservoirId = "cold";
			loop.coolantCapacityKg = 1; loop.startingCoolantKg = 1; loop.specificHeatKJPerKgK = 1;
			loop.startingTemperatureC = 5.001; loop.freezeProtectionSetpointC = 5;
			loop.pumpPowerKW = 1; loop.ratedFlowKgPerS = 1;
			loop.exchangerConductanceKWPerK = 1; loop.heaterCapacityKW = 10;
			loop.criticalService = true; s.coolantLoops = {loop};
			core::SolarArraySettings solar; solar.id = "sun"; solar.capacityKW = shortage ? 0.5 : 20;
			solar.illumination.constantLevel = 1; s.solarArrays = {solar};
			core::StationSystemsRuntime runtime(s, quantum);
			require(runtime.advanceTo(6.0 * quantum), "Freeze prediction failed");
			auto const snap = runtime.snapshot();
			auto const& result = snap.coolantLoops.front();
			require(result.heaterDemandKW == 10, "Predicted freeze did not request heat before crossing");
			if (!shortage)
				require(result.loopTemperatureC >= 5 && result.heaterSuppliedKW == 10, "Available freeze protection reacted late");
			else require(snap.power.protectedUnmetKW > 0 && result.heaterSuppliedKW == 0,
				"Freeze protection fabricated supply or hid protected unmet demand");
			require(std::abs(result.residualKJ) < 1e-9, "Freeze protection lost energy");
			std::cout << "freeze shortage=" << shortage << " temperatureC=" << result.loopTemperatureC
				<< " heaterRequestedKW=" << result.heaterDemandKW << " heaterSuppliedKW=" << result.heaterSuppliedKW
				<< " protectedUnmetKW=" << snap.power.protectedUnmetKW << '\n';
		}
	}

	void rateAwareProtection(smoke::Context const&)
	{
		core::ScopedStationStepRatioForTesting explicitClock(std::nullopt);
		std::cout << "Protection certified ratio=" << core::StationProtectionCertifiedStepRatio
			<< " thermalOvershootBoundC=" << core::StationProtectionTemperatureOvershootC << '\n';
		for (auto const& bound : core::StationProtectionBounds)
			require(bound.overshoot >= 0 && !bound.unit.empty() && !bound.basis.empty()
				&& !bound.verifiedBy.empty() && !bound.evidence.empty(), "Incomplete protection registry entry");
		for (int k = Generator; k <= Chiller; ++k)
		{
			auto const kind = static_cast<Kind>(k);
			for (bool cold : {false, true})
			{
				if (cold && kind != Microbial && kind != Crop && kind != Algae) continue;
				core::StationSystemsRuntime control(fixture(kind, 6, false, cold), quantum);
				require(control.advanceTo(60.0 * quantum), "Control drive failed");
				require(production(control.snapshot(), kind) > 0, std::string(names[k]) + " control did no work");
				for (uint32_t ratio = 1; ratio <= core::StationProtectionCertifiedStepRatio; ++ratio)
				{
					auto settings = fixture(kind, ratio, true, cold);
					core::StationSystemsRuntime runtime(settings, quantum), partitioned(settings, quantum);
					require(runtime.advanceTo(quantum / 2.0), "Initial protection drive failed");
					auto const firstProduction = production(runtime.snapshot(), kind);
					if (ratio > 1) require(firstProduction == 0, std::string(names[k]) + " admitted work across its initial predicted limit");
					require(runtime.advanceTo(60.0 * quantum), "Protection drive failed");
					for (double target : {30.0 * quantum, 60.0 * quantum})
						require(partitioned.advanceTo(target), "Partitioned protection drive failed");
					auto const s = runtime.snapshot();
					require(s == partitioned.snapshot() && runtime.events() == partitioned.events(), "Trial state leaked or drive partition changed protection");
					if (ratio > 1 && !cold) require(production(s, kind) == 0, std::string(names[k]) + " admitted work across its predicted limit");
					for (auto const& r : s.thermal.reservoirs) require(std::abs(r.residualKJ) < 1e-8, "Protection lost thermal energy");
					std::cout << names[k] << (cold ? ".minimum" : ".maximum") << " ratio=" << ratio << " firstProduction=" << firstProduction << " production=" << production(s, kind) << '\n';
				}
			}
		}
		for (uint32_t ratio = 2; ratio <= core::StationProtectionCertifiedStepRatio; ++ratio)
		{
			auto s = fixture(Chiller, ratio, false);
			auto& loop = s.coolantLoops.front();
			loop.coolantCapacityKg = 0.001; loop.startingCoolantKg = 0.001;
			loop.specificHeatKJPerKgK = 1; loop.startingTemperatureC = 5.001;
			s.chillers.front().chilledSetpointC = 5;
			core::StationSystemsRuntime runtime(s, quantum);
			for (double target : {6.0 * quantum, 60.0 * quantum})
			{
				require(runtime.advanceTo(target), "Chiller cold-side drive failed");
				auto const snap = runtime.snapshot();
				require(snap.coolantLoops.front().loopTemperatureC + core::StationProtectionTemperatureOvershootC >= 5,
					"Coarse chiller over-extracted its finite cold-side energy");
				require(std::abs(snap.coolantLoops.front().residualKJ) < 1e-8, "Chiller cold-side energy did not close");
			}
		}
	}
}

void registerStationProtectionChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"rateAwareProtection", rateAwareProtection});
	checks.push_back({"protectionReallocation", reallocation});
	checks.push_back({"protectionFreeze", freeze});
	checks.push_back({"protectionGrossExcursion", grossExcursion});
}
