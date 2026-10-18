#include "MaterialObservations.h"
#include "WaterGasObservationFixture.h"
#include "CarbonObservationFixture.h"
#include "MicrobialObservationFixture.h"
#include "CropObservationFixture.h"
#include "AlgaeObservationFixture.h"
#include "ScheduledObservationFixture.h"
#include "core/World.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace
{
	void require(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }
	void close(double actual, double expected)
	{
		require(std::isfinite(actual) && std::abs(actual - expected) < 1e-8, "material inventory numerical mismatch");
	}
	core::ResourceStoreSettings store(std::string id, std::string unit, double capacity,
		double qualified, double unqualified, std::string material = {})
	{
		core::ResourceStoreSettings s;
		s.id = std::move(id); s.unit = std::move(unit); s.capacity = capacity;
		s.startingQualified = qualified; s.startingUnqualified = unqualified; s.materialId = std::move(material);
		return s;
	}
	core::StationSystemsSettings rig()
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		// Labels are deliberately misleading. The second store resolves only
		// through the validated, material-preserving transfer relationship.
		s.stores = {store("oxygen", "kg", 100, 10, 5, "water"), store("spare", "kg", 100, 0, 0),
			store("grams", "g", 10000, 1000, 1000, "water"), store("water", "kg", 100, 3, 1, "oxygen"),
			store("hazard", "kg", 100, 2, 1, "water"), store("mystery", "kg", 100, 7, 2),
			store("litres", "L", 10000, 1000, 500, "water")};
		s.stores[4].hazardClass = "hazardous";
		s.stores[2].composition = core::ResourceCompositionSettings{0.25, 0.1};
		s.stores[5].composition = core::ResourceCompositionSettings{0.1, 1};
		s.stores[0].protectedReserve = 2;
		s.transfers = {{"move", "oxygen", "spare", "kg", 2}};
		return s;
	}
	core::StationChartDefinition chart(core::World const& world, std::string const& id)
	{
		auto const catalogue = world.getStationSystemsObservationCatalogue();
		auto found = std::find_if(catalogue.begin(), catalogue.end(), [&](auto const& c) { return c.id == id; });
		require(found != catalogue.end(), "expected material property missing");
		return *found;
	}
	void run(core::World& world, uint64_t ticks = 60)
	{
		world.resumeSimulation(); require(world.advanceTicks(ticks), "material observation run failed"); world.pauseSimulation();
	}
	void exchangeObservations(smoke::Context const&)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		core::ResourceConversionSettings conversion; conversion.id = "split";
		for (size_t i = 0; i < 5; ++i)
		{
			auto feed = store("feed" + std::to_string(i), "kg", 100, 50, 0, "feedstock");
			auto product = store("product" + std::to_string(i), "g", 100000, 0, 0, i == 4 ? "other-product" : "product");
			feed.composition = core::ResourceCompositionSettings{}; product.composition = feed.composition;
			s.stores.push_back(feed); s.stores.push_back(product);
			conversion.inputs.push_back({feed.id, 2}); conversion.outputs.push_back({product.id, 1000});
		}
		s.conversions = {conversion};
		s.stores.push_back(store("source", "kg", 100, 20, 0, "water"));
		s.stores.push_back(store("destination", "kg", 100, 0, 0, "water"));
		s.stores.push_back(store("dry", "kg", 100, 0, 0, "water"));
		s.stores.push_back(store("empty", "kg", 100, 0, 0, "water"));
		// Deliberately reverse priority order relative to authored order.
		s.transfers = {{"idle", "dry", "empty", "kg", 2, 10}, {"move", "source", "destination", "kg", 2, 0}};
		s.connections[{"Transfer", "move", "Source", "Source", "Store", "source"}] = {core::StationPhysicalConnection::Automatic, 1, 0.5};
		s.connections[{"Conversion", "split", "Input 1", "Input 1", "Store", "feed0"}] = {core::StationPhysicalConnection::Automatic, 1, 0.5};
		core::World world("Exchanges", 2, 2); world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		if (!world.configureStationSystems(s, false, &diagnostic)) throw std::runtime_error(diagnostic);
		auto const definitions = world.getStationSystemsObservationCatalogue();
		size_t outputs = 0;
		for (auto const& c : definitions) if (c.group == "Resource exchanges" && c.breakdown == "Aggregate"
			&& c.id.find("Resource conversion/Output/") != std::string::npos) ++outputs;
		require(outputs == 5, "five outputs did not yield independent selectable properties");
		run(world);
		require(world.getStationSystemsQueryWork().queries == 0, "hidden exchanges performed chart queries");
		auto const snap = world.getStationSystemsSnapshot();
		auto const revision = world.getStationSystemsObservationRevision();
		std::string const transferId = "exchange/Resource transfer/Output/Destination/water/kg/s/normal";
		auto const transfer = world.queryStationSystemsChart(transferId);
		require(transfer.series.size() == 2 && transfer.series[0].values.size() == 10,
			"exchange default is not aggregate actual with requested overlay and retained history");
		for (size_t row = 0; row < transfer.series[0].values.size(); ++row)
		{ close(transfer.series[0].values[row], 0.5); close(transfer.series[1].values[row], 4); }
		auto const instances = world.queryStationSystemsChart(transferId + "/instances");
		require(instances.series.size() == 4 && !instances.series.back().stacked, "requests stacked into actual contributions");
		for (size_t row = 0; row < instances.series[0].values.size(); ++row)
		{ close(instances.series[1].values[row], 0); close(instances.series[2].values[row], 0.5); }
		auto const loss = world.queryStationSystemsChart("exchange/Resource transfer/Transport loss/Source/water/kg/s/normal");
		require(loss.series.size() == 1, "transport loss manufactured a request");
		close(loss.series[0].values[0], 0.5);
		auto const input = world.queryStationSystemsChart("exchange/Resource transfer/Input/Source/water/kg/s/normal");
		require(input.series.size() == 1, "transfer input manufactured authoritative request");
		close(input.series[0].values[0], 0.5);
		for (size_t i = 0; i < 5; ++i)
		{
			auto const id = "exchange/Resource conversion/Output/Output " + std::to_string(i + 1)
				+ "/" + (i == 4 ? "other-product" : "product") + "/kg/s/normal";
			auto const data = world.queryStationSystemsChart(id);
			require(data.series.size() == 2, "growable output lost request or canonical material grouping");
			close(data.series[0].values[0], 0.25); close(data.series[1].values[0], 1);
		}
		auto const histories = world.getStationSystemsMetricHistory();
		auto missing = histories;
		std::erase_if(missing, [](auto const& h) { return h.descriptor.instanceId == "idle"; });
		auto const gap = core::queryStationChart(chart(world, transferId), missing);
		require(gap.incomplete && std::isnan(gap.series[0].values[0]), "missing exchange became zero");
		require(world.getStationSystemsSnapshot() == snap && world.getStationSystemsObservationRevision() == revision,
			"exchange query changed simulation");
		// Missing requested authority is a gap in the overlay, not actual material.
		auto missingRequest = histories;
		for (auto& h : missingRequest) if (h.descriptor.equipmentType == "Resource transfer"
			&& h.descriptor.instanceId == "move" && h.descriptor.role == core::StationMetricRole::Output)
			h.samples[0].requested.reset();
		auto const requestGap = core::queryStationChart(chart(world, transferId), missingRequest);
		require(std::isfinite(requestGap.series[0].values[0]) && std::isnan(requestGap.series[1].values[0]),
			"missing request contaminated actual or became zero");
		// Hazard/material constraints remain authoritative even with observations installed.
		auto invalid = s; invalid.stores[0].hazardClass = "hazardous";
		require(!world.configureStationSystems(invalid, true), "exchange views bypassed hazard enforcement");
		// Catalogue grouping must not conflate equal role labels and units of different materials.
		auto descriptors = std::vector<core::StationMetricDescriptor>{};
		for (auto const& h : histories) if (h.descriptor.equipmentType == "Resource conversion") descriptors.push_back(h.descriptor);
		auto other = descriptors.front(); other.id += "/other"; other.materialId = "different"; descriptors.push_back(other);
		auto const grouped = core::stationCharts(descriptors);
		require(std::any_of(grouped.begin(), grouped.end(), [](auto const& c) { return c.id.find("/different/") != std::string::npos; }),
			"mixed materials sharing labels and units were conflated");
		// An inactive conversion has measured zero, not unavailable output.
		for (size_t i = 0; i < 5; ++i) s.stores[2 * i].startingQualified = 0;
		require(world.configureStationSystems(s, true), "inactive conversion configuration failed");
		run(world, 6);
		auto const inactive = world.queryStationSystemsChart("exchange/Resource conversion/Output/Output 5/other-product/kg/s/normal");
		close(inactive.series[0].values[0], 0); close(inactive.series[1].values[0], 1);
		// Canonical rate scales are independent of names and observation cadence.
		for (auto const& [unit, scale, canonical] : std::vector<std::tuple<std::string, double, core::StationCanonicalUnit>>{
			{"g", 0.001, core::StationCanonicalUnit::KilogramPerSecond},
			{"L", 0.001, core::StationCanonicalUnit::CubicMetrePerSecond},
			{"Wh", 3.6, core::StationCanonicalUnit::Kilowatt},
			{"mol", 1, core::StationCanonicalUnit::MolePerSecond}})
		{
			core::StationSystemsSettings metadata;
			metadata.stores = {store("a", unit, 100, 10, 0, "material"), store("b", unit, 100, 0, 0, "material")};
			metadata.transfers = {{"t", "a", "b", unit, 1}};
			auto const observations = core::stationExchangeObservations(metadata);
			require(observations.size() == 4 && observations[0].metric.unit == canonical, "canonical rate unit missing");
			close(observations[0].sourceToCanonicalScale, scale);
		}
	}

	void scheduledExchangeObservations(smoke::Context const&)
	{
		auto s = smoke::station::scheduledObservationFixture();
		core::World world("Scheduled exchanges", 2, 2); world.pauseSimulation(); world.finishBuild();
		std::string why; if (!world.configureStationSystems(s, true, &why)) throw std::runtime_error(why);
		auto const catalogue = world.getStationSystemsObservationCatalogue();
		require(std::count_if(catalogue.begin(), catalogue.end(), [](auto const& c) {
			return c.breakdown == "Aggregate" && (c.id.starts_with("exchange/Resupply/")
				|| c.id.starts_with("exchange/Waste ejection/") || c.id.starts_with("exchange/Maintenance/"));
		}) == 5, "scheduled exchanges unavailable before first observation");
		run(world, 60);
		require(world.getStationSystemsQueryWork().queries == 0, "hidden event recording queried charts");
		auto const snap = world.getStationSystemsSnapshot();
		auto const histories = world.getStationSystemsMetricHistory();
		size_t count = 0;
		for (auto const& o : core::stationExchangeObservations(s)) {
			if (o.metric.equipmentType != "Resupply" && o.metric.equipmentType != "Waste ejection" && o.metric.equipmentType != "Maintenance") continue;
			++count; require(o.cumulativeRate && !o.perTick && !o.metric.requestedSemantics, "event treated as tick rate or invented request");
			auto h = std::ranges::find(histories, o.metric.id, [](auto const& h) { return h.descriptor.id; });
			require(h != histories.end() && h->samples.size() == 5, "event history missing before selection");
			double total = 0, previous = 0;
			for (auto const& v : h->samples) {
				require(v.actual.has_value() && !v.requested, "scheduled measurement unavailable");
				total += *v.actual * (v.elapsedSeconds - previous); previous = v.elapsedSeconds;
			}
			close(total, *o.read(snap).actual * o.sourceToCanonicalScale);
			auto absent = snap; absent.deliveries.clear(); absent.wasteEjections.clear(); absent.maintenanceJobs.clear();
			require(!o.read(absent).actual, "missing schedule authority became zero");
			if (o.metric.equipmentType != "Resupply" || o.metric.exchangeRole == "Accepted") close(*h->samples.back().actual, 0);
		}
		require(count == 8, "missing scheduled metrics or reusable tools graphed as consumption");
		auto get = [&](std::string id) { auto d = world.queryStationSystemsChart(id); require(!d.incomplete && !d.series.empty(), "event chart missing"); return d; };
		std::string const accepted = "exchange/Resupply/Input/Accepted/water/kg/s/normal";
		auto data = get(accepted); close(data.series[0].values[0], 0.005 / data.series[0].times[0]);
		// First window includes time zero, two recurrences and the exact-boundary one-off.
		close(get("exchange/Resupply/Input/Delivered/water/kg/s/normal").series[0].values[0], 0.007 / data.series[0].times[0]);
		close(get("exchange/Resupply/Output/Rejected/water/kg/s/normal").series[0].values[0], 0.002 / data.series[0].times[0]);
		auto stack = get(accepted + "/instances"); close(stack.series.back().upper[0], data.series[0].values[0]);
		close(snap.stores[0].currentQualified, 5);
		close(snap.stores[1].currentQualified, 0); close(snap.stores[1].currentUnqualified, 4);
		close(snap.wasteEjections[0].ejectedTotal, 2); require(snap.wasteEjections[0].unmetTotal > 0, "held waste was ejected");
		close(snap.stores[2].currentQualified, 0.5); close(snap.stores[3].currentQualified, 1); close(snap.stores[4].currentQualified, 2);
		close(snap.maintenanceJobs[0].requirements[1].consumedTotal, 2);
		require(snap.maintenanceJobs[0].occurrencesCompleted == 2, "material-limited maintenance completed without resources");
		// Inventories are existing owners, not stock plus event throughput.
		close(get("stored-material/water/kg/aggregate").series[0].values.back(), 0.005);
		close(get("stored-material/parts/kg/aggregate").series[0].values.back(), 2.5);
		auto const revision = world.getStationSystemsObservationRevision();
		require(world.getStationSystemsSnapshot() == snap, "event viewing changed simulation");
		world.pauseSimulation(); require(!world.advanceTicks(12) && world.getStationSystemsObservationRevision() == revision, "pause recorded events");
		auto missing = histories;
		for (auto& h : missing) if (h.descriptor.id == "exchange/Resupply/boundary/Accepted") h.samples[0].actual.reset();
		auto gap = core::queryStationChart(chart(world, accepted), missing);
		require(gap.incomplete && std::isnan(gap.series[0].values[0]), "missing event contribution hidden by aggregate");
		world.resetSimulation(); run(world, 60);
		require(world.getStationSystemsMetricHistory().size() == histories.size(), "event reset changed catalogue");
		auto replay = world.queryStationSystemsChart(accepted);
		require(replay.series[0].values == data.series[0].values, "event windows not deterministic on replay");
		require(world.getStationSystemsSnapshot() == snap, "scheduled ledgers drifted on reset");
		for (int mode : {1, 2}) {
			auto treatment = smoke::station::scheduledObservationFixture(mode);
			core::World gated("Treatment-gated ejection", 2, 2); gated.pauseSimulation(); gated.finishBuild();
			if (!gated.configureStationSystems(treatment, true, &why)) throw std::runtime_error(why);
			run(gated, 60);
			auto observed = gated.getStationSystemsSnapshot();
			auto ejected = gated.queryStationSystemsChart("exchange/Waste ejection/Loss/Ejected material/water/kg/s/hazardous");
			require(!ejected.incomplete && !ejected.series.empty(), "treatment-gated ejection not graphed");
			double total = 0, previous = 0;
			for (size_t i = 0; i < ejected.series[0].times.size(); ++i) {
				total += ejected.series[0].values[i] * (ejected.series[0].times[i] - previous); previous = ejected.series[0].times[i];
			}
			close(total, observed.wasteEjections[1].ejectedTotal);
			if (mode == 1) {
				close(total, 0); require(observed.stores[7].currentUnqualified > 0, "failed clearance did not hold treatment output");
			} else require(total > 0, "passed treatment clearance never permitted ejection");
		}
		// Schedules without physical spend do not fabricate material exchanges.
		s.deliveries.clear(); s.wasteEjections.clear(); s.maintenanceJobs[0].requirements.pop_back(); s.maintenanceJobs[0].wasteStoreId.clear();
		for (auto const& o : core::stationExchangeObservations(s)) require(o.metric.equipmentType != "Resupply"
			&& o.metric.equipmentType != "Waste ejection" && o.metric.equipmentType != "Maintenance", "non-consuming schedule manufactured a graph");
	}

	void algaeExchangeObservations(smoke::Context const&)
	{
		for (int scenario = 0; scenario < 13; ++scenario) {
			auto s = smoke::station::algaeObservationFixture(scenario < 4 ? scenario : (scenario == 8 ? 1 : 0));
			if (scenario == 4) s.stores[0].startingQualified = 0;
			if (scenario == 5) s.algaeCultures[0].illuminationProfile.constantLevel = 0;
			if (scenario == 6) s.algaeCultures[0].cultureVolumeL = 0, s.algaeCultures[0].initialBiomassKg = 0;
			if (scenario == 7) s.solarArrays[0].capacityKW = 0;
			if (scenario == 8) s.stores[5].capacity = 1e-6; // Food surplus cascades to recovery.
			if (scenario == 9) s.algaeCultures.push_back(s.algaeCultures[0]), s.algaeCultures.back().id = "second";
			if (scenario == 10) {
				s.atmospheres[0].hazardClass = "hazardous";
				for (auto& store : s.stores) store.hazardClass = "hazardous";
			}
			if (scenario == 11) s.solarArrays[0].capacityKW = 1.9; // Partial light, not a zero-growth case.
			if (scenario == 12) s.stores[1].startingQualified = 0;
			core::World world("Algae exchanges", 2, 2); world.pauseSimulation(); world.finishBuild();
			std::string why; if (!world.configureStationSystems(s, true, &why)) throw std::runtime_error(why);
			run(world, 300);
			auto const snap = world.getStationSystemsSnapshot(); auto const& p = snap.algaeCultures[0];
			auto const history = world.getStationSystemsMetricHistory(); auto const catalogue = world.getStationSystemsObservationCatalogue();
			auto const dt = core::World::getFixedTimestep();
			size_t measured = 0;
			for (auto const& o : core::stationExchangeObservations(s)) if (o.metric.equipmentType == "Algae") {
				++measured;
				require(o.metric.hazardClass == s.atmospheres[0].hazardClass, "algae hazard grouping lost");
				auto h = std::ranges::find(history, o.metric.id, [](auto const& h) { return h.descriptor.id; });
				require(h != history.end() && !h->samples.empty(), "algae metric not recorded while hidden");
				auto v = o.read(snap); require(v.actual.has_value(), "algae authority missing");
				auto missing = snap; missing.algaeCultures.clear(); require(!o.read(missing).actual, "missing algae became zero");
				if (o.cumulativeRate) {
					double total = 0, previous = 0;
					for (auto const& sample : h->samples) {
						require(sample.actual.has_value(), "algae event window unavailable");
						total += *sample.actual * (sample.elapsedSeconds - previous); previous = sample.elapsedSeconds;
					}
					close(total, *v.actual);
				} else close(*h->samples.back().actual, *v.actual / (o.perTick ? dt : 1));
				if (v.requested) close(*h->samples.back().requested, *v.requested / (o.perTick ? dt : 1));
				require(v.requested.has_value() == (o.metric.exchangeRole == "Lamp electrical service"
					|| o.metric.exchangeRole == "Circulation electrical service"), "invented algae request overlay");
			}
			require(measured == (scenario == 3 ? 35 : scenario == 9 ? 98 : 49), "algae role inventory incomplete");
			for (auto const& def : catalogue) if (def.breakdown == "Aggregate" && def.id.starts_with("exchange/Algae/")) {
				auto data = world.queryStationSystemsChart(def.id); require(!data.incomplete && !data.series[0].values.empty(), "algae chart incomplete");
				auto stack = world.queryStationSystemsChart(def.id + "/instances");
				auto last = std::find_if(stack.series.rbegin(), stack.series.rend(), [](auto const& l) { return l.stacked; });
				require(last != stack.series.rend(), "algae stack missing"); close(last->upper.back(), data.series[0].values.back());
				auto unavailable = history;
				for (auto& h : unavailable) if (h.descriptor.id == def.series[0].contributors[0]) h.samples.back().actual.reset();
				auto gap = core::queryStationChart(def, unavailable);
				require(gap.incomplete && std::isnan(gap.series[0].values.back()), "missing algae hidden by aggregate");
			}
			close(s.algaeCultures[0].initialBiomassKg + p.growthKg - p.respirationKg - p.harvestedKg
				- p.deadResidueToWasteKg - p.deadResidueLostKg, p.livingBiomassKg);
			if (scenario != 9) {
				close(s.stores[0].startingQualified - snap.stores[0].currentQualified, p.waterDrawnKg);
				close(snap.stores[5].currentQualified, p.foodProducedKg);
				close(snap.stores[6].currentQualified, p.wasteProducedKg + p.deadResidueToWasteKg);
				close(snap.stores[7].currentQualified, p.recoveryProducedKg);
			}
			close(p.co2DrawnKg + p.waterDrawnKg + p.nitrogenDrawnKg + p.phosphorusDrawnKg + p.potassiumDrawnKg + p.mineralsDrawnKg
				+ p.respirationO2DrawnKg - p.oxygenReleasedKg - p.respirationCo2ReturnedKg - p.respirationWaterKg - p.evaporatedKg,
				p.livingBiomassKg - s.algaeCultures[0].initialBiomassKg + p.harvestedKg + p.deadResidueToWasteKg + p.deadResidueLostKg);
			// Accounted inventories deliberately advertise partial station-wide coverage;
			// require a finite measurement, not a claim of complete physical accounting.
			auto inventory = [&](std::string const& id) {
				auto data = world.queryStationSystemsChart(id);
				if (data.series.empty() || data.series[0].values.empty()) throw std::runtime_error("algae inventory contribution missing: " + id);
				require(std::isfinite(data.series[0].values.back()), "algae inventory contribution unavailable");
				return data.series[0].values.back();
			};
			double biomass = 0, livingEnergy = 0, boundNitrogen = 0;
			for (auto const& culture : snap.algaeCultures) {
				biomass += culture.livingBiomassKg; livingEnergy += culture.livingChemicalEnergyKJ;
				boundNitrogen += culture.livingConstituents.nutrientNitrogenKg;
			}
			close(inventory("accounted-material/living-biomass/kg"), biomass);
			close(inventory("accounted-constituent/energy/kWh"), inventory("stored-constituent/energy/kWh") + livingEnergy / 3600);
			close(inventory("accounted-constituent/nutrientNitrogen/kg"), inventory("stored-constituent/nutrientNitrogen/kg") + boundNitrogen);
			close(p.harvestedKg, p.foodProducedKg + p.recoveryProducedKg + p.wasteProducedKg);
			for (auto const& [ledger, mass] : {std::pair{p.harvestedToFoodConstituents, p.foodProducedKg},
				std::pair{p.harvestedToRecoveryConstituents, p.recoveryProducedKg}, std::pair{p.harvestedToWasteConstituents, p.wasteProducedKg},
				std::pair{p.deadResidueToWasteConstituents, p.deadResidueToWasteKg}, std::pair{p.deadResidueLostConstituents, p.deadResidueLostKg}})
				close(ledger.organicKg + ledger.nutrientNitrogenKg + ledger.phosphorusKg + ledger.potassiumKg + ledger.mineralsKg, mass);
			if (scenario == 0 || scenario == 9) require(p.growthKg > 0, "algae growth not exercised");
			if (scenario == 1) require(p.foodProducedKg > 0 && p.recoveryProducedKg > 0 && p.wasteProducedKg > 0, "three-way harvest not exercised");
			if (scenario == 2 || scenario == 3) require(p.dead && p.respirationKg > 0 && (scenario == 2 ? p.deadResidueToWasteKg : p.deadResidueLostKg) > 0, "algae mortality not exercised");
			if (scenario >= 4 && scenario <= 7) close(p.growthKg, 0);
			if (scenario == 8) require(p.foodSurplusToRecoveryKg > 0 && p.recoveryProducedKg > 0, "algae harvest cascade not exercised");
			if (scenario == 10) require(p.growthKg > 0, "hazardous algae growth not exercised");
			if (scenario == 11) require(p.growthKg > 0 && p.lastLightSuppliedKW < p.lastLightRequestedKW, "partial algae service not exercised");
			if (scenario == 12) close(p.growthKg, 0);
			require(world.getStationSystemsSnapshot() == snap, "algae viewing changed World");
			world.resetSimulation(); run(world, 300); require(world.getStationSystemsSnapshot() == snap, "algae observation replay drift");
		}
	}

	void cropExchangeObservations(smoke::Context const&)
	{
		for (int scenario = 0; scenario < 8; ++scenario) {
			auto s = smoke::station::cropObservationFixture(scenario < 4 ? scenario : 0);
			if (scenario == 4) s.stores[0].startingQualified = 0;
			if (scenario == 5) s.cropPopulations[0].illuminationProfile.constantLevel = 0;
			if (scenario == 6) s.cropPopulations[0].growingAreaM2 = 0, s.cropPopulations[0].initialBiomassKg = 0;
			if (scenario == 7) s.solarArrays[0].capacityKW = 0;
			core::World world("Crop exchanges", 2, 2); world.pauseSimulation(); world.finishBuild();
			std::string why; if (!world.configureStationSystems(s, true, &why)) throw std::runtime_error(why);
			run(world, 300);
			auto const snap = world.getStationSystemsSnapshot(); auto const& p = snap.cropPopulations[0];
			auto const history = world.getStationSystemsMetricHistory(); auto const catalogue = world.getStationSystemsObservationCatalogue();
			auto const dt = core::World::getFixedTimestep();
			for (auto const& o : core::stationExchangeObservations(s)) if (o.metric.equipmentType == "Crops") {
				auto h = std::ranges::find(history, o.metric.id, [](auto const& h) { return h.descriptor.id; });
				require(h != history.end() && !h->samples.empty(), "crop metric not recorded while hidden");
				auto v = o.read(snap); require(v.actual.has_value(), "crop authority missing");
				auto missing = snap; missing.cropPopulations.clear(); require(!o.read(missing).actual, "missing crop became zero");
				if (o.cumulativeRate) {
					double total = 0, previous = 0;
					for (auto const& sample : h->samples) {
						require(sample.actual.has_value(), "crop event window unavailable");
						total += *sample.actual * (sample.elapsedSeconds - previous); previous = sample.elapsedSeconds;
					}
					close(total, *v.actual); // Includes events on non-observation ticks.
				} else close(*h->samples.back().actual, *v.actual / (o.perTick ? dt : 1));
				if (v.requested) close(*h->samples.back().requested, *v.requested / (o.perTick ? dt : 1));
			}
			for (auto const& def : catalogue) if (def.breakdown == "Aggregate" && def.id.starts_with("exchange/Crops/")) {
				auto data = world.queryStationSystemsChart(def.id); require(!data.incomplete && !data.series[0].values.empty(), "crop chart incomplete");
				auto stack = world.queryStationSystemsChart(def.id + "/instances");
				auto last = std::find_if(stack.series.rbegin(), stack.series.rend(), [](auto const& l) { return l.stacked; });
				require(last != stack.series.rend(), "crop stack missing"); close(last->upper.back(), data.series[0].values.back());
				auto unavailable = history;
				for (auto& h : unavailable) if (h.descriptor.id == def.series[0].contributors[0]) h.samples.back().actual.reset();
				auto gap = core::queryStationChart(def, unavailable);
				require(gap.incomplete && std::isnan(gap.series[0].values.back()), "missing crop hidden by aggregate");
			}
			close(s.cropPopulations[0].initialBiomassKg + p.growthKg - p.respirationKg - p.harvestedKg
				- p.deadResidueToWasteKg - p.deadResidueLostKg, p.livingBiomassKg);
			close(s.stores[0].startingQualified - snap.stores[0].currentQualified, p.waterDrawnKg);
			close(snap.stores[5].currentQualified, p.foodProducedKg);
			close(snap.stores[6].currentQualified, p.wasteProducedKg + p.deadResidueToWasteKg);
			close(p.co2DrawnKg + p.waterDrawnKg + p.nitrogenDrawnKg + p.phosphorusDrawnKg + p.potassiumDrawnKg + p.mineralsDrawnKg
				+ p.respirationO2DrawnKg - p.oxygenReleasedKg - p.respirationCo2ReturnedKg - p.respirationWaterKg - p.transpiredKg,
				p.livingBiomassKg - s.cropPopulations[0].initialBiomassKg + p.harvestedKg + p.deadResidueToWasteKg + p.deadResidueLostKg);
			if (scenario == 0) require(p.growthKg > 0, "growth not exercised");
			if (scenario == 1) require(p.harvestedKg > 0 && p.harvestedToFoodConstituents.nutrientNitrogenKg > 0, "harvest not exercised");
			if (scenario == 2 || scenario == 3) require(p.dead && p.respirationKg > 0 && (scenario == 2 ? p.deadResidueToWasteKg : p.deadResidueLostKg) > 0, "mortality not exercised");
			if (scenario >= 4) close(p.growthKg, 0);
			require(world.getStationSystemsSnapshot() == snap, "crop viewing changed World");
			world.resetSimulation(); run(world, 300); require(world.getStationSystemsSnapshot() == snap, "crop observation replay drift");
		}
	}

	void microbialExchangeObservations(smoke::Context const&)
	{
		auto s = smoke::station::microbialObservationFixture();
		core::World world("Microbial exchanges", 2, 2); world.pauseSimulation(); world.finishBuild();
		auto apply = [&] { std::string why; if (!world.configureStationSystems(s, true, &why)) throw std::runtime_error(why); };
		auto check = [&](std::string const& role, double expected) {
			for (auto const& def : world.getStationSystemsObservationCatalogue()) if (def.breakdown == "Aggregate"
				&& def.id.starts_with("exchange/Microbial processor/") && def.id.find("/" + role + "/") != std::string::npos) {
				auto const data = world.queryStationSystemsChart(def.id);
				require(!data.incomplete && !data.series[0].values.empty(), "microbial chart missing history");
				close(data.series[0].values.back(), expected); return;
			}
			throw std::runtime_error("missing microbial role " + role);
		};
		auto verify = [&] {
			auto const snap = world.getStationSystemsSnapshot(); auto const history = world.getStationSystemsMetricHistory();
			auto missing = snap; missing.microbialProcessors.clear(); missing.connections.clear();
			for (auto const& o : core::stationExchangeObservations(s)) if (o.metric.equipmentType == "Microbial processor") {
				auto h = std::ranges::find(history, o.metric.id, [](auto const& h) { return h.descriptor.id; });
				require(h != history.end() && !h->samples.empty(), "microbial metric absent");
				auto v = o.read(snap); require(v.actual && h->samples.back().actual, "microbial exchange unavailable");
				close(*h->samples.back().actual, *v.actual * o.sourceToCanonicalScale / (o.perTick ? core::World::getFixedTimestep() : 1));
				if (v.requested) close(*h->samples.back().requested, *v.requested * o.sourceToCanonicalScale / (o.perTick ? core::World::getFixedTimestep() : 1));
				require(!o.read(missing).actual, "missing microbial authority became zero");
				if (o.metric.exchangeRole == "Recovered nutrients") require(o.metric.materialId != "nitrogen" && !o.metric.requestedSemantics, "nutrient product confused with atmospheric nitrogen or requested output");
			}
			for (auto const& def : world.getStationSystemsObservationCatalogue()) if (def.breakdown == "Aggregate" && def.id.starts_with("exchange/Microbial processor/")) {
				auto const data = world.queryStationSystemsChart(def.id);
				require(!data.incomplete && !data.series[0].values.empty(), "microbial aggregate unavailable");
				auto const stack = world.queryStationSystemsChart(def.id + "/instances");
				auto last = std::find_if(stack.series.rbegin(), stack.series.rend(), [](auto const& l) { return l.stacked; });
				require(last != stack.series.rend(), "microbial instance stack missing"); close(last->upper.back(), data.series[0].values.back());
				auto unavailable = history;
				for (auto& h : unavailable) if (h.descriptor.id == def.series[0].contributors[0]) h.samples.back().actual.reset();
				auto const gap = core::queryStationChart(def, unavailable);
				require(gap.incomplete && std::isnan(gap.series[0].values.back()), "missing microbial contribution concealed");
			}
		};
		apply(); run(world, 12); verify();
		auto snap = world.getStationSystemsSnapshot(); auto const dt = core::World::getFixedTimestep(); auto p = snap.microbialProcessors[0];
		require(p.lastFeedConsumedKg > 0 && p.lastStockConsumedUnits > 0 && p.condition > s.microbialProcessors[0].initialCondition, "microbial recovery fixture inactive");
		check("Organic feed", p.lastFeedConsumedKg / dt); check("Recovered nutrients", p.lastNutrientProducedKg / dt);
		check("Produced heat", p.lastPowerSuppliedKW + p.lastReactionHeatKW);
		check("Replacement stock", p.lastStockConsumedUnits / dt);
		check("Oxygen", p.lastOxygenConsumedKg / dt); check("Process water", p.lastWaterConsumedKg / dt);
		check("Recovered water", p.lastRecoveredWaterKg / dt); check("CO2", p.lastCo2ProducedKg / dt);
		check("Unrecoverable residue", p.lastResidueProducedKg / dt);
		close(p.lastFeedConsumedKg + p.lastOxygenConsumedKg + p.lastWaterConsumedKg,
			p.lastNutrientProducedKg + p.lastRecoveredWaterKg + p.lastCo2ProducedKg + p.lastResidueProducedKg);
		close(p.nutrientProducedKg, p.nitrogenRecoveredKg + p.phosphorusRecoveredKg + p.potassiumRecoveredKg + p.mineralsRecoveredKg);
		// A real oxygen shortage limits processing, without a graph-side simulation.
		s.stores[1].startingQualified = 0; apply(); run(world, 12); verify(); check("Organic feed", 0);
		// Exhausted medium degrades condition and reports known inactivity.
		s = smoke::station::microbialObservationFixture(); s.stores[3].startingQualified = 0;
		apply(); run(world, 12); verify(); check("Culture medium", 0); check("Recovered nutrients", 0);
		require(world.getStationSystemsSnapshot().microbialProcessors[0].condition < s.microbialProcessors[0].initialCondition, "medium exhaustion did not degrade culture");
		// Cold stress and benign recovery both preserve the committed exchange rates.
		s = smoke::station::microbialObservationFixture(); s.thermalReservoirs[0].startingTemperatureC = 10;
		apply(); run(world, 12); verify(); check("Organic feed", 0);
		require(world.getStationSystemsSnapshot().microbialProcessors[0].condition < s.microbialProcessors[0].initialCondition, "cold stress not exercised");
		s = smoke::station::microbialObservationFixture(); apply(); run(world, 12); verify(); snap = world.getStationSystemsSnapshot();
		for (auto const& c : snap.connections) if (c.key.ownerType == "Microbial processor") {
			if (c.key.relation == "Oxygen") s.connections[c.key] = {core::StationPhysicalConnection::Automatic, 0.0005, 0.5};
			if (c.key.relation == "Heat") s.connections[c.key] = {core::StationPhysicalConnection::Automatic, 0.000001, 0.5};
		}
		apply(); run(world, 12); verify(); bool loss = false, retained = false;
		for (auto const& c : world.getStationSystemsSnapshot().connections) if (c.key.ownerType == "Microbial processor") {
			close(c.lastInputRate, c.lastDeliveredRate + c.lastLossRate);
			loss |= c.lastLossRate > 0; retained |= c.retainedHeatKJ > 0;
		}
		require(loss && retained, "microbial transport loss and retained heat not exercised");
		s = smoke::station::microbialObservationFixture(); s.microbialProcessors[0].rateKgPerS = 0;
		apply(); run(world, 12); verify(); check("Organic feed", 0); check("Produced heat", 0);
		// Consumables are in declared store units, unlike intrinsic kg process accounts.
		s = smoke::station::microbialObservationFixture(); s.stores[3].unit = "g"; s.stores[8].unit = "g";
		apply(); run(world, 12); verify(); p = world.getStationSystemsSnapshot().microbialProcessors[0];
		check("Culture medium", p.lastConsumableUsed * 0.001 / dt); check("Replacement stock", p.lastStockConsumedUnits * 0.001 / dt);
		// Multiple instances aggregate only compatible material identities.
		s = smoke::station::microbialObservationFixture(); auto second = s.microbialProcessors[0]; second.id = "second"; s.microbialProcessors.push_back(second);
		apply(); run(world, 12); verify(); snap = world.getStationSystemsSnapshot();
		check("Organic feed", (snap.microbialProcessors[0].lastFeedConsumedKg + snap.microbialProcessors[1].lastFeedConsumedKg) / dt);
		for (auto& stock : s.stores) stock.hazardClass = "hazardous";
		for (auto& processor : s.microbialProcessors) processor.hazardClass = "hazardous";
		apply(); run(world, 12); verify();
		for (auto const& h : world.getStationSystemsMetricHistory()) if (h.descriptor.equipmentType == "Microbial processor")
			require(h.descriptor.hazardClass == "hazardous", "microbial hazard metadata lost");
		// Exchange queries preserve the generic hazard-segregated view boundary.
		s = smoke::station::microbialObservationFixture(); auto hazardous = s.microbialProcessors[0];
		hazardous.id = "hazardous"; hazardous.hazardClass = "hazardous";
		for (auto member : {&core::MicrobialProcessorSettings::feedStoreId, &core::MicrobialProcessorSettings::oxygenStoreId,
			&core::MicrobialProcessorSettings::waterStoreId, &core::MicrobialProcessorSettings::consumableStoreId,
			&core::MicrobialProcessorSettings::stockStoreId, &core::MicrobialProcessorSettings::nutrientStoreId,
			&core::MicrobialProcessorSettings::recoveredWaterStoreId, &core::MicrobialProcessorSettings::co2StoreId,
			&core::MicrobialProcessorSettings::residueStoreId}) hazardous.*member += "-hazardous";
		auto hazardousStores = s.stores;
		for (auto& stock : hazardousStores) { stock.id += "-hazardous"; stock.hazardClass = "hazardous"; s.stores.push_back(stock); }
		s.microbialProcessors.push_back(hazardous); apply(); run(world, 12); verify(); size_t hazards = 0;
		for (auto const& def : world.getStationSystemsObservationCatalogue()) if (def.breakdown == "Aggregate"
			&& def.id.starts_with("exchange/Microbial processor/") && def.id.find("/Organic feed/") != std::string::npos) {
			// Each hazard has exactly its own processor, not the other chain.
			require(def.series[0].contributors.size() == 1, "microbial hazards merged"); ++hazards;
		}
		require(hazards == 2, "microbial hazard-specific views missing");
		// Unknown compound identity is an honest incomplete chart, not guessed
		// from the feed's name, composition, or compatible kg unit.
		s = smoke::station::microbialObservationFixture(); s.stores[0].materialId.clear();
		apply(); run(world, 12);
		bool unclassified = false;
		for (auto const& def : world.getStationSystemsObservationCatalogue()) if (def.breakdown == "Aggregate"
			&& def.id.starts_with("exchange/Microbial processor/") && def.id.find("/Organic feed/") != std::string::npos) {
			auto const data = world.queryStationSystemsChart(def.id);
			require(data.incomplete && std::isnan(data.series[0].values.back()), "unknown microbial feed silently classified"); unclassified = true;
		}
		require(unclassified, "unclassified microbial feed disappeared");
	}

	void carbonExchangeObservations(smoke::Context const&)
	{
		auto s = smoke::station::carbonObservationFixture();
		core::World world("Carbon exchanges", 2, 2); world.pauseSimulation(); world.finishBuild();
		auto apply = [&] { std::string why; if (!world.configureStationSystems(s, true, &why)) throw std::runtime_error(why); };
		auto family = [](std::string const& t) { return t == "Sabatier" || t == "Methane processor" || t == "Fuel cell"; };
		auto check = [&](std::string const& type, std::string const& role, double expected) {
			for (auto const& def : world.getStationSystemsObservationCatalogue()) if (def.breakdown == "Aggregate"
				&& def.id.starts_with("exchange/" + type + "/") && def.id.find("/" + role + "/") != std::string::npos) {
				auto const data = world.queryStationSystemsChart(def.id);
				require(!data.incomplete && !data.series[0].values.empty(), "carbon chart missing history");
				close(data.series[0].values.back(), expected);
				auto const stack = world.queryStationSystemsChart(def.id + "/instances");
				auto last = std::find_if(stack.series.rbegin(), stack.series.rend(), [](auto const& l) { return l.stacked; });
				require(last != stack.series.rend(), "carbon instance stack missing"); close(last->upper.back(), expected);
				return;
			}
			throw std::runtime_error("missing carbon role " + type + "/" + role);
		};
		auto verify = [&] {
			auto const snap = world.getStationSystemsSnapshot();
			auto const history = world.getStationSystemsMetricHistory();
			auto missing = snap; missing.sabatierReactors.clear(); missing.methaneProcessors.clear(); missing.fuelCells.clear(); missing.connections.clear();
			for (auto const& o : core::stationExchangeObservations(s)) if (family(o.metric.equipmentType) && o.read) {
				auto h = std::ranges::find(history, o.metric.id, [](auto const& h) { return h.descriptor.id; });
				require(h != history.end() && !h->samples.empty(), "carbon metric missing");
				auto const v = o.read(snap);
				require(v.actual.has_value() && h->samples.back().actual.has_value(), "committed carbon exchange unavailable");
				close(*h->samples.back().actual, *v.actual * o.sourceToCanonicalScale / (o.perTick ? core::World::getFixedTimestep() : 1));
				if (v.requested) close(*h->samples.back().requested, *v.requested * o.sourceToCanonicalScale / (o.perTick ? core::World::getFixedTimestep() : 1));
				require(!o.read(missing).actual, "missing carbon authority became zero");
			}
			for (auto const& def : world.getStationSystemsObservationCatalogue()) if (def.breakdown == "Aggregate" && def.id.starts_with("exchange/")
				&& std::any_of(history.begin(), history.end(), [&](auto const& h) { return family(h.descriptor.equipmentType) && std::ranges::find(def.series[0].contributors, h.descriptor.id) != def.series[0].contributors.end(); })) {
				auto const data = world.queryStationSystemsChart(def.id);
				require(!data.incomplete, "carbon aggregate incomplete");
				auto unavailable = history;
				for (auto& h : unavailable) if (h.descriptor.id == def.series[0].contributors[0]) h.samples.back().actual.reset();
				auto const gap = core::queryStationChart(def, unavailable);
				require(gap.incomplete && std::isnan(gap.series[0].values.back()), "missing carbon contributor concealed");
			}
		};
		apply(); run(world, 12); verify();
		auto snap = world.getStationSystemsSnapshot(); auto dt = core::World::getFixedTimestep();
		require(snap.sabatierReactors[0].lastCo2ConsumedKg > 0 && snap.methaneProcessors[0].lastMethaneConsumedKg > 0 && snap.fuelCells[0].lastOutputKW > 0, "carbon fixture inactive");
		check("Sabatier", "CO2 feed", snap.sabatierReactors[0].lastCo2ConsumedKg / dt);
		check("Sabatier", "Methane", snap.sabatierReactors[0].lastMethaneProducedKg / dt);
		check("Sabatier", "Water", snap.sabatierReactors[0].lastWaterProducedKg / dt);
		check("Sabatier", "Produced heat", snap.sabatierReactors[0].lastPowerSuppliedKW + snap.sabatierReactors[0].lastReactionHeatKW);
		check("Methane processor", "Solid carbon", snap.methaneProcessors[0].lastCarbonProducedKg / dt);
		check("Methane processor", "Recovered hydrogen", snap.methaneProcessors[0].lastHydrogenReturnedKg / dt);
		check("Methane processor", "Produced heat", snap.methaneProcessors[0].lastPowerSuppliedKW - snap.methaneProcessors[0].lastReactionHeatKW);
		check("Fuel cell", "Generation", snap.fuelCells[0].lastOutputKW);
		check("Fuel cell", "Production waste heat", snap.fuelCells[0].lastWasteHeatKW);
		// Physical connection input caps and losses remain authoritative.
		for (auto const& c : snap.connections) if (family(c.key.ownerType)) s.connections[c.key] = {core::StationPhysicalConnection::Automatic, 0.0005, 0.5};
		apply(); run(world, 12); verify(); snap = world.getStationSystemsSnapshot(); bool loss = false;
		for (auto const& c : snap.connections) if (family(c.key.ownerType) && c.lastInputRate > 0) {
			close(c.lastInputRate, c.lastDeliveredRate + c.lastLossRate); loss |= c.lastLossRate > 0;
		}
		require(loss && snap.sabatierReactors[0].lastCo2ConsumedKg / dt < s.sabatierReactors[0].rateKgPerS, "carbon reactant constraints not exercised");
		// Dedicated oxygen exhaustion cannot borrow breathing oxygen.
		s.connections.clear(); s.stores[6].startingQualified = 0;
		apply(); run(world, 12); verify(); check("Fuel cell", "Dedicated oxygen", 0); check("Fuel cell", "Generation", 0);
		close(world.getStationSystemsSnapshot().stores[8].currentQualified, 10);
		s.sabatierReactors[0].rateKgPerS = 0; s.methaneProcessors[0].rateKgPerS = 0; s.fuelCells[0].ratedOutputKW = s.fuelCells[0].commandedOutputKW = 0;
		apply(); run(world, 12); verify(); check("Sabatier", "Methane", 0); check("Methane processor", "Solid carbon", 0);
		// Real run through coordinated history coarsening; constant rates must survive.
		s = smoke::station::carbonObservationFixture(); auto second = s.sabatierReactors[0]; second.id = "second"; s.sabatierReactors.push_back(second);
		apply(); run(world, 1800);
		auto const status = world.getStationSystemsRecordingStatus(); require(status.coarseningLevel > 0, "carbon history did not coarsen");
		auto const before = world.getStationSystemsSnapshot(); auto const revision = world.getStationSystemsObservationRevision();
		for (auto const& def : world.getStationSystemsObservationCatalogue()) if (def.breakdown == "Aggregate" && def.id.starts_with("exchange/Sabatier/Output/Methane/")) {
			auto const data = world.queryStationSystemsChart(def.id); auto const stack = world.queryStationSystemsChart(def.id + "/instances");
			for (size_t row = 0; row < data.series[0].values.size(); ++row) {
				close(data.series[0].values[row], 2 * 0.001 * core::StationSabatierMethaneMassPerCarbonDioxideKg);
				close(stack.series.back().upper[row], data.series[0].values[row]);
				close(stack.series.back().times[row], data.series[0].times[row]);
			}
		}
		require(world.getStationSystemsSnapshot() == before && world.getStationSystemsObservationRevision() == revision, "carbon query mutated World");
		auto invalid = s; invalid.stores[0].hazardClass = "hazardous";
		require(!world.configureStationSystems(invalid, true), "carbon observation bypassed hazard compatibility");
		s = smoke::station::carbonObservationFixture();
		for (auto& stock : s.stores) stock.hazardClass = "hazardous";
		s.sabatierReactors[0].hazardClass = s.methaneProcessors[0].hazardClass = s.fuelCells[0].hazardClass = "hazardous";
		apply(); run(world, 12); verify();
		for (auto const& h : world.getStationSystemsMetricHistory()) {
			auto const& d = h.descriptor;
			if (d.equipmentType == "Sabatier" || d.equipmentType == "Methane processor") {
				require(d.hazardClass == "hazardous", "carbon hazard identity lost");
				if (d.exchangeRole == "Produced heat") require(d.thermalClass == "moderate", "carbon thermal class lost");
				if (d.exchangeRole == "Methane") require(d.materialId == "methane" && !d.requestedSemantics, "methane identity or invented product request");
				if (d.exchangeRole == "Solid carbon") require(d.materialId == "carbon" && !d.requestedSemantics, "carbon identity or invented product request");
			}
		}
		s.enabled = false; apply(); run(world, 12);
		for (auto const& h : world.getStationSystemsMetricHistory()) if (family(h.descriptor.equipmentType)) require(h.samples.empty(), "disabled carbon history fabricated");
	}

	void waterGasExchangeObservations(smoke::Context const&)
	{
		auto s = smoke::station::waterGasObservationFixture();
		core::World world("Water gas exchanges", 2, 2); world.pauseSimulation(); world.finishBuild();
		auto apply = [&] { std::string why; if (!world.configureStationSystems(s, true, &why)) throw std::runtime_error(why); };
		auto family = [](std::string const& t) { return t == "Water collector" || t == "Water treatment" || t == "Electrolyzer" || t == "CO2 capture"; };
		auto verify = [&] {
			auto const snap = world.getStationSystemsSnapshot();
			auto const history = world.getStationSystemsMetricHistory();
			auto missing = snap; missing.waterCollectors.clear(); missing.waterTreatments.clear(); missing.electrolyzers.clear(); missing.co2Captures.clear(); missing.connections.clear();
			for (auto const& o : core::stationExchangeObservations(s)) if (family(o.metric.equipmentType)) {
				auto h = std::ranges::find(history, o.metric.id, [](auto const& h) { return h.descriptor.id; });
				require(h != history.end() && !h->samples.empty() && h->samples.back().actual.has_value(), "water/gas metric absent");
				auto v = o.read(snap);
				close(*h->samples.back().actual, *v.actual * o.sourceToCanonicalScale / (o.perTick ? core::World::getFixedTimestep() : 1));
				if (v.requested) close(*h->samples.back().requested, *v.requested * o.sourceToCanonicalScale / (o.perTick ? core::World::getFixedTimestep() : 1));
				require(!o.read(missing).actual, "missing water/gas authority became zero");
			}
			auto const before = world.getStationSystemsSnapshot();
			auto const revision = world.getStationSystemsObservationRevision();
			for (auto const& def : world.getStationSystemsObservationCatalogue()) if (def.breakdown == "Aggregate" && std::any_of(history.begin(), history.end(), [&](auto const& h) {
				return family(h.descriptor.equipmentType) && std::ranges::find(def.series[0].contributors, h.descriptor.id) != def.series[0].contributors.end();
			})) {
				auto const data = world.queryStationSystemsChart(def.id);
				require(!data.incomplete && !data.series[0].values.empty(), "water/gas chart unavailable");
				double expected = 0;
				for (auto const& id : def.series[0].contributors) {
					auto h = std::ranges::find(history, id, [](auto const& h) { return h.descriptor.id; }); expected += *h->samples.back().actual;
				}
				close(data.series[0].values.back(), expected);
				auto const stack = world.queryStationSystemsChart(def.id + "/instances");
				auto lastStack = std::find_if(stack.series.rbegin(), stack.series.rend(), [](auto const& line) { return line.stacked; });
				require(lastStack != stack.series.rend() && !lastStack->upper.empty(), "water/gas instance stack missing");
				close(lastStack->upper.back(), expected);
				auto unavailable = history;
				for (auto& h : unavailable) if (h.descriptor.id == def.series[0].contributors[0]) h.samples.back().actual.reset();
				auto gap = core::queryStationChart(def, unavailable);
				require(gap.incomplete && std::isnan(gap.series[0].values.back()), "unavailable contributor concealed in total");
			}
			require(world.getStationSystemsSnapshot() == before && world.getStationSystemsObservationRevision() == revision, "viewing water/gas changed World");
		};
		apply(); run(world, 12); verify();
		auto snap = world.getStationSystemsSnapshot();
		require(snap.waterCollectors[0].lastCollectedKg > 0, ("collector inactive: " + snap.waterCollectors[0].blockReason + " request=" + std::to_string(snap.waterCollectors[0].lastRequestedKg) + " latent=" + std::to_string(snap.waterCollectors[0].lastLatentHeatKW) + " treatment=" + std::to_string(snap.waterTreatments[0].lastProcessedKg)).c_str());
		require(snap.waterTreatments[0].lastProcessedKg > 0, ("treatment inactive: " + snap.waterTreatments[0].limiter).c_str());
		require(snap.electrolyzers[0].lastSplitKg > 0, ("electrolyzer inactive: " + snap.electrolyzers[0].blockReason).c_str());
		require(snap.co2Captures[0].lastCapturedKg > 0, ("capture inactive: " + snap.co2Captures[0].blockReason).c_str());
		// Snapshot-to-query numerical checks independent of the adapter callbacks.
		auto check = [&](std::string const& type, std::string const& role, double actual) {
			for (auto const& def : world.getStationSystemsObservationCatalogue()) if (def.breakdown == "Aggregate"
				&& def.id.starts_with("exchange/" + type + "/") && def.id.find("/" + role + "/") != std::string::npos) {
				close(world.queryStationSystemsChart(def.id).series[0].values.back(), actual); return;
			}
			throw std::runtime_error("missing water/gas role " + type + "/" + role);
		};
		auto dt = core::World::getFixedTimestep();
		check("Water collector", "Condensate", snap.waterCollectors[0].lastCollectedKg / dt);
		check("Water treatment", "Concentrate", snap.waterTreatments[0].lastConcentrateKg / dt);
		check("Electrolyzer", "Oxygen", snap.electrolyzers[0].lastOxygenKg / dt);
		check("Electrolyzer", "Product chemical energy", snap.electrolyzers[0].lastHydrogenKg * core::StationElectrolysisChemicalEnergyHydrogenKJPerKg / dt);
		check("CO2 capture", "Captured CO2", snap.co2Captures[0].lastCapturedKg / dt);
		// Constrained physical inputs/outputs expose separately named external losses.
		for (auto const& c : snap.connections) if (family(c.key.ownerType))
			s.connections[c.key] = {core::StationPhysicalConnection::Automatic, 0.005, 0.5};
		apply(); run(world, 12); verify(); snap = world.getStationSystemsSnapshot();
		bool loss = false;
		for (auto const& c : snap.connections) if (family(c.key.ownerType) && c.lastInputRate > 0) {
			close(c.lastInputRate, c.lastDeliveredRate + c.lastLossRate); loss |= c.lastLossRate > 0;
		}
		require(loss, "connection loss not exercised");
		// Same-class instances participate in the generic aggregate/stack.
		s.connections.clear(); auto second = s.electrolyzers[0]; second.id = "second"; s.electrolyzers.push_back(second);
		apply(); run(world, 12); verify();
		s.waterCollectors[0].maxRateKgPerS = 0; s.waterTreatments[0].throughputKgPerS = 0;
		for (auto& e : s.electrolyzers) e.rateKgPerS = 0;
		s.co2Captures[0].maxRateKgPerS = 0; apply(); run(world, 12); verify();
		check("Electrolyzer", "Oxygen", 0); check("Water collector", "Condensate", 0);
		check("Water treatment", "Treated water", 0); check("CO2 capture", "Captured CO2", 0);
		// Hazardous clearance and non-kg consumables retain explicit identity and
		// canonical rates. Oxygen may also go directly to atmosphere, not a store.
		s = smoke::station::waterGasObservationFixture();
		for (auto& stock : s.stores) stock.hazardClass = "hazardous";
		s.atmospheres[0].hazardClass = "hazardous"; s.coolantLoops[0].hazardClass = "hazardous";
		s.waterCollectors[0].hazardClass = "hazardous"; s.waterTreatments[0].hazardClass = "hazardous";
		s.electrolyzers[0].hazardClass = "hazardous"; s.co2Captures[0].hazardClass = "hazardous";
		s.waterTreatments[0].clearanceStoreId = "kit"; s.waterTreatments[0].clearancePerKg = 0.01;
		s.stores[3].unit = "g"; s.stores[3].capacity = 100000; s.stores[3].startingQualified = 10000;
		s.electrolyzers[0].oxygenStoreId.clear(); s.electrolyzers[0].atmosphereId = "air";
		auto otherKit = s.stores[3]; otherKit.id = "other-kit"; otherKit.materialId = "other-sorbent"; s.stores.push_back(otherKit);
		auto otherCapture = s.co2Captures[0]; otherCapture.id = "other-capture"; otherCapture.consumableStoreId = "other-kit"; s.co2Captures.push_back(otherCapture);
		apply(); run(world, 12); verify(); snap = world.getStationSystemsSnapshot();
		check("Water treatment", "Clearance consumable", snap.waterTreatments[0].lastClearanceUsed * 0.001 / dt);
		check("Electrolyzer", "Oxygen", snap.electrolyzers[0].lastOxygenKg / dt);
		size_t consumableViews = 0;
		for (auto const& def : world.getStationSystemsObservationCatalogue()) if (def.breakdown == "Aggregate" && def.id.starts_with("exchange/CO2 capture/Input/Consumable/")) {
			++consumableViews;
			require(def.series[0].contributors.size() == 1 && def.id.ends_with("/hazardous"), "material/hazard identity pooled");
		}
		require(consumableViews == 2, "same-unit different consumables merged");
		s.enabled = false; apply(); run(world, 12);
		for (auto const& h : world.getStationSystemsMetricHistory()) if (family(h.descriptor.equipmentType))
			require(h.samples.empty(), "disabled simulation fabricated water/gas history");
	}

	void crewAtmosphereExchangeObservations(smoke::Context const&)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		core::ResourceCompositionSettings food, water, urine, waste;
		food.moisture = 0.8; food.nutrientNitrogen = 0.05; food.phosphorus = 0.01;
		food.potassium = 0.02; food.otherMinerals = 0.02; food.energyKJPerKgDry = 17000;
		water.moisture = 0.999; water.otherMinerals = 0.001;
		urine.moisture = 0.95; urine.nutrientNitrogen = 0.1; urine.phosphorus = 0.02;
		urine.potassium = 0.05; urine.otherMinerals = 0.05; urine.energyKJPerKgDry = 500;
		waste.moisture = 0.5; waste.energyKJPerKgDry = 10000;
		s.stores = {store("food", "g", 100000, 50000, 0, "food"), store("water", "kg", 100, 50, 0, "water"),
			store("urine", "kg", 100, 0, 0, "urine"), store("waste", "kg", 100, 0, 0, "organic-waste")};
		s.stores[0].composition = food; s.stores[1].composition = water;
		s.stores[2].composition = urine; s.stores[3].composition = waste;
		core::AtmosphereSettings a; a.id = "habitat"; a.hazardClass = "normal"; a.volumeM3 = 100;
		a.startingOxygenKg = 20; a.startingNitrogenKg = 80; a.thermalCapacityKJPerK = 10000;
		a.minPressureKPa = 1; a.maxPressureKPa = 200; a.leakRateKgPerS = 0.01;
		a.occupants = 1; a.oxygenUsePerOccupantKgPerS = 0.001; a.co2ProductionPerOccupantKgPerS = 0.001;
		s.atmospheres = {a};
		auto hazardous = a; hazardous.id = "hazard"; hazardous.hazardClass = "hazardous"; s.atmospheres.push_back(hazardous);
		core::CrewSettings c; c.id = "crew"; c.atmosphereId = a.id; c.crewCount = 2; c.activityProfile.constantLevel = 1;
		c.foodStoreId = "food"; c.waterStoreId = "water"; c.urineStoreId = "urine"; c.organicWasteStoreId = "waste";
		c.foodPerCrewPerS = 1; c.waterPerCrewPerS = 0.002; c.oxygenPerCrewKgPerS = 0.001;
		c.co2PerCrewKgPerS = 0.0011; c.moisturePerCrewKgPerS = 0.0005;
		c.urinePerCrewPerS = 0.0015; c.organicWastePerCrewPerS = 0.0002; c.metabolicHeatPerCrewKW = 0.1;
		s.crews = {c};
		// Off-cadence occurrence followed by several occurrences in one window.
		s.airlockCycles = {{"cycle", a.id, 0.2, 0.025, 0.025, 0.25}};
		core::World world("Crew exchanges", 2, 2); world.pauseSimulation(); world.finishBuild();
		auto apply = [&] { std::string why; if (!world.configureStationSystems(s, true, &why)) throw std::runtime_error(why); };
		apply(); run(world, 12);
		auto const snap = world.getStationSystemsSnapshot();
		auto const observations = core::stationExchangeObservations(s);
		auto history = world.getStationSystemsMetricHistory();
		auto value = [&](std::string const& id) {
			auto h = std::ranges::find(history, id, [](auto const& h) { return h.descriptor.id; });
			require(h != history.end() && !h->samples.empty() && h->samples.back().actual.has_value(), "crew/air observation missing");
			return *h->samples.back().actual;
		};
		close(value("exchange/Crew/crew/Food"), 0.002);
		close(value("exchange/Crew/crew/Oxygen"), 0.002);
		close(value("exchange/Atmosphere/habitat/Leakage"), 0.01);
		close(value("exchange/Crew/crew/Metabolic heat"), 0.2);
		close(value("exchange/Crew/crew/Retained body mass"), snap.crews[0].retainedKg);
		close(value("exchange/Airlock cycle/cycle/Retained recovered mixture"), snap.airlockCycles[0].recoveredTotal);
		require(snap.airlockCycles[0].occurrencesCompleted > 2, "multiple off-cadence events not exercised");
		close(value("exchange/Crew/crew/Food") + value("exchange/Crew/crew/Water") + value("exchange/Crew/crew/Oxygen"),
			value("exchange/Crew/crew/Carbon dioxide") + value("exchange/Crew/crew/Moisture")
			+ value("exchange/Crew/crew/Urine") + value("exchange/Crew/crew/Organic waste") + 0.0014);
		for (auto const& o : observations) if (o.metric.equipmentType == "Crew" || o.metric.equipmentType == "Atmosphere" || o.metric.equipmentType == "Airlock cycle")
		{
			auto missing = snap; missing.crews.clear(); missing.atmospheres.clear(); missing.airlockCycles.clear();
			require(!o.read(missing).actual, "missing exchange became zero");
			if (!o.cumulativeRate) close(value(o.metric.id), *o.read(snap).actual * o.sourceToCanonicalScale / (o.perTick ? core::World::getFixedTimestep() : 1));
			else {
				auto h = std::ranges::find(history, o.metric.id, [](auto const& h) { return h.descriptor.id; });
				double amount = 0, previous = 0;
				for (auto const& sample : h->samples) { amount += *sample.actual * (sample.elapsedSeconds - previous); previous = sample.elapsedSeconds; }
				close(amount, *o.read(snap).actual);
			}
		}
		auto const before = world.getStationSystemsSnapshot(); auto const revision = world.getStationSystemsObservationRevision();
		bool eventChart = false;
		for (auto const& def : world.getStationSystemsObservationCatalogue()) if (def.breakdown == "Aggregate" && def.id.starts_with("exchange/Atmosphere/") && def.id.find("/Leakage/") != std::string::npos)
		{
			require(def.series[0].contributors.size() == 1 && def.id.find("atmosphere-mixture") != std::string::npos, "hazards or species pooled in leakage");
		}
		for (auto const& def : world.getStationSystemsObservationCatalogue()) if (def.id.starts_with("exchange/Crew/") || def.id.starts_with("exchange/Airlock cycle/"))
		{
			auto data = world.queryStationSystemsChart(def.id); require(!data.incomplete, "generic crew chart incomplete");
			if (def.breakdown == "Aggregate" && def.id.find("/Food/") != std::string::npos)
			{
				require(data.series.size() == 2, "authoritative request overlay missing"); close(data.series[1].values.back(), 0.002);
				auto missing = history;
				for (auto& h : missing) if (h.descriptor.id == def.series[0].contributors[0]) h.samples.back().actual.reset();
				auto gap = core::queryStationChart(def, missing);
				require(gap.incomplete && std::isnan(gap.series[0].values.back()), "unavailable crew amount became zero");
			}
			if (def.id.starts_with("exchange/Airlock cycle/") && def.quantity == core::StationPhysicalQuantity::MassRate)
			{ eventChart = true; require(def.accountingNote.find("Interval-average event rate") != std::string::npos, "event window not documented"); }
		}
		require(eventChart && world.getStationSystemsSnapshot() == before && world.getStationSystemsObservationRevision() == revision, "query changed World");
		// Reapplication starts fresh baselines and repeats exactly, rather than differencing across runs.
		auto const firstRate = value("exchange/Airlock cycle/cycle/Removed gas");
		apply(); run(world, 12); history = world.getStationSystemsMetricHistory();
		close(value("exchange/Airlock cycle/cycle/Removed gas"), firstRate);
		// No new occurrence after a one-off: cumulative ledger remains, rate returns to zero.
		s.airlockCycles[0].recurrenceSeconds = 0; s.crews[0].crewCount = 0; apply(); run(world, 18);
		history = world.getStationSystemsMetricHistory();
		close(value("exchange/Airlock cycle/cycle/Removed gas"), 0);
		close(value("exchange/Crew/crew/Food"), 0);
	}

	void thermalExchangeObservations(smoke::Context const&)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		s.solarArrays = {{"sun", 100}};
		for (auto const* cls : {"warm", "moderate"})
		{
			core::ThermalReservoirSettings r; r.id = cls; r.thermalClass = cls;
			r.heatCapacityKJPerK = 10000; r.startingTemperatureC = 20; r.minTemperatureC = -50; r.maxTemperatureC = 100;
			r.absorbedHeatKW = 5; s.thermalReservoirs.push_back(r);
			auto sink = r; sink.id += "-sink"; s.thermalReservoirs.push_back(sink);
			core::CoolantLoopSettings l; l.id = cls; l.reservoirId = cls; l.hazardClass = "normal";
			l.coolantCapacityKg = 100; l.startingCoolantKg = 100; l.specificHeatKJPerKgK = 4;
			l.startingTemperatureC = 20; l.freezeProtectionSetpointC = 30; l.heaterCapacityKW = 1;
			l.pumpPowerKW = 1; l.ratedFlowKgPerS = 2; l.exchangerConductanceKWPerK = 1;
			s.coolantLoops.push_back(l);
			core::ChillerSettings c; c.id = std::string(cls) + "-chiller"; c.thermalClass = cls; c.coldLoopId = cls; c.condenserReservoirId = std::string(cls) + "-sink";
			c.ratedCoolingCapacityKW = 6; c.cop = 3; c.auxiliaryPowerKW = 0.5; c.chilledSetpointC = 5; c.maxCondensingTemperatureC = 90;
			s.chillers.push_back(c);
		}
		s.radiators = {{"radiator", "warm", 1}};
		core::ThermalBufferSettings b; b.id = "buffer"; b.reservoirId = "warm"; b.accessMode = "passive";
		b.capacityKJ = 1000; b.minUsableTemperatureC = 0; b.maxUsableTemperatureC = 40;
		b.maxChargeKW = 2; b.maxDischargeKW = 2; b.transferConductanceKWPerK = 1;
		s.thermalBuffers = {b};
		core::World world("Thermal exchanges", 2, 2); world.pauseSimulation(); world.finishBuild();
		auto apply = [&] { std::string why; if (!world.configureStationSystems(s, true, &why)) throw std::runtime_error(why); };
		apply(); run(world, 6);
		auto verify = [&] {
			auto const snapshot = world.getStationSystemsSnapshot();
			auto const observations = core::stationExchangeObservations(s);
			auto const history = world.getStationSystemsMetricHistory();
			size_t checked = 0;
			for (auto const& o : observations) if (o.metric.thermalClass)
			{
				require(!o.metric.materialId, "thermal exchange depends on material identity");
				auto h = std::ranges::find(history, o.metric.id, [](auto const& a) { return a.descriptor.id; });
				require(h != history.end() && !h->samples.empty() && h->samples.back().actual.has_value()
					&& h->samples.back().validity == core::StationMetricValidity::Valid, "thermal history missing or invalid");
				auto expected = o.read(snapshot);
				require(expected.actual.has_value(), "thermal endpoint missing");
				close(*h->samples.back().actual, *expected.actual * o.sourceToCanonicalScale);
				if (expected.requested) close(*h->samples.back().requested, *expected.requested);
				auto missing = snapshot;
				missing.thermal.reservoirs.clear(); missing.thermal.radiators.clear(); missing.coolantLoops.clear();
				missing.chillers.clear(); missing.thermalBuffers.clear(); missing.connections.clear();
				require(!o.read(missing).actual, "missing thermal owner became zero"); ++checked;
			}
			require(checked > 50, "thermal family catalogue incomplete");
		};
		verify();
		auto definition = [&](std::string type, std::string role, std::string cls) {
			auto catalogue = world.getStationSystemsObservationCatalogue();
			auto c = std::find_if(catalogue.begin(), catalogue.end(), [&](auto const& c) {
				return c.breakdown == "Aggregate" && c.id.starts_with("exchange/" + type + "/")
					&& c.id.find("/" + role + "/") != std::string::npos && c.id.ends_with("/thermal-class/" + cls);
			});
			require(c != catalogue.end(), "class-segregated thermal chart missing"); return *c;
		};
		auto const warm = definition("Chiller", "Cooling provided", "warm");
		auto const cold = definition("Chiller", "Cooling provided", "moderate");
		require(warm.id != cold.id && warm.series[0].contributors.size() == 1, "cooling classes pooled");
		auto snap = world.getStationSystemsSnapshot();
		require(snap.chillers[0].coolingKW > 0 && snap.coolantLoops[0].heaterDemandKW > 0
			&& snap.thermalBuffers[0].lastChargeKW > 0, "fixture missed active thermal operation");
		close(world.queryStationSystemsChart(warm.id).series[0].values.back(), snap.chillers[0].coolingKW);
		auto pump = world.queryStationSystemsChart(definition("Coolant loop", "Pump electricity", "warm").id);
		close(pump.series[0].values.back(), snap.coolantLoops[0].pumpSuppliedKW);
		close(pump.series[1].values.back(), snap.coolantLoops[0].pumpDemandKW);
		auto heater = world.queryStationSystemsChart(definition("Coolant loop", "Heater electricity", "warm").id);
		close(heater.series[0].values.back(), snap.coolantLoops[0].heaterSuppliedKW);
		close(heater.series[1].values.back(), snap.coolantLoops[0].heaterDemandKW);
		auto const stored = definition("Thermal buffer", "Stored thermal energy", "warm");
		require(stored.unit == core::StationCanonicalUnit::KilowattHour
			&& stored.group == "Equipment inventories" && stored.series[0].label == "Inventory state", "inventory mislabeled as useful output");
		close(world.queryStationSystemsChart(stored.id).series[0].values.back(), snap.thermalBuffers[0].storedEnergyKJ / 3600);
		auto histories = world.getStationSystemsMetricHistory();
		for (auto& h : histories) if (h.descriptor.id == warm.series[0].contributors[0]) h.samples.back().actual.reset();
		auto gap = core::queryStationChart(warm, histories);
		require(gap.incomplete && !gap.series[0].values.empty() && std::isnan(gap.series[0].values.back()), "missing cooling became zero");
		// Apply real physical throughput/efficiency limits; all chart endpoints
		// must follow the allocator, not unconstrained equipment calculations.
		for (auto const& c : snap.connections) if (c.key.ownerType == "Chiller" || c.key.ownerType == "Coolant loop"
			|| c.key.ownerType == "Radiator" || c.key.ownerType == "Thermal buffer")
			s.connections[c.key] = {core::StationPhysicalConnection::Automatic, 2, 0.5};
		apply(); run(world, 6); verify();
		snap = world.getStationSystemsSnapshot();
		for (auto const& c : snap.connections) if (s.connections.contains(c.key) && c.lastInputRate > 0)
		{ require(c.lastInputRate <= 2 + 1e-8, "thermal throughput exceeded"); close(c.lastDeliveredRate, c.lastInputRate * 0.5); }
		// A second same-class instance participates in generic breakdowns.
		s.connections.clear(); auto second = s.chillers[0]; second.id = "second"; second.coldLoopId = "second";
		auto loop = s.coolantLoops[0]; loop.id = "second-loop"; second.coldLoopId = loop.id; s.coolantLoops.push_back(loop); s.chillers.push_back(second);
		apply(); run(world, 6); verify();
		auto aggregate = definition("Chiller", "Cooling provided", "warm");
		auto instances = world.queryStationSystemsChart(aggregate.id + "/instances");
		require(instances.series.size() == 4, "thermal instance breakdown missing");
		close(instances.series[1].values.back() + instances.series[2].values.back(), world.queryStationSystemsChart(aggregate.id).series[0].values.back());
		auto missingMember = world.getStationSystemsMetricHistory();
		std::erase_if(missingMember, [&](auto const& h) { return h.descriptor.id == aggregate.series[0].contributors[0]; });
		auto missingTotal = core::queryStationChart(aggregate, missingMember);
		require(missingTotal.incomplete && !missingTotal.series[0].values.empty()
			&& std::isnan(missingTotal.series[0].values.back()), "missing class member silently shrank total");
		// Disabled equipment is valid measured zero. Buffer can return stored heat.
		s.chillers[0].ratedCoolingCapacityKW = 0; s.coolantLoops[1].startingCoolantKg = 0;
		s.thermalBuffers[0].startingEnergyKJ = 1000; s.radiators[0].areaM2 = 100000;
		apply(); run(world, 6); verify(); snap = world.getStationSystemsSnapshot();
		close(snap.chillers[0].coolingKW, 0); close(snap.coolantLoops[1].pumpSuppliedKW, 0);
		if (!(snap.thermalBuffers[0].lastDischargeKW > 0)) throw std::runtime_error("buffer discharge not exercised: " + snap.thermalBuffers[0].lastLimiter
			+ "; stored=" + std::to_string(snap.thermalBuffers[0].storedEnergyKJ)
			+ "; rejected=" + std::to_string(snap.thermal.radiators[0].lastRejectedKW));
		auto const before = snap; auto const revision = world.getStationSystemsObservationRevision();
		world.queryStationSystemsChart(definition("Thermal buffer", "Stored thermal energy", "warm").id);
		require(world.getStationSystemsSnapshot() == before && world.getStationSystemsObservationRevision() == revision, "thermal query mutated run");
	}

	void electricalExchangeObservations(smoke::Context const&)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		s.solarArrays = {{"sun", 20}};
		s.batteries = {{"bank", 10, 10, 5, 5, 10, 0.8, 0.7}};
		core::EquipmentLoadSettings load; load.id = "load"; load.electricalDemandKW = 4;
		load.thermalClass = "warm"; load.coolingDisposition = core::StationCoolingDisposition::Exported;
		load.heatOutputKW = 2; load.materialExchanges = {{"misleading-oxygen", 2, true}, {"return", 1, false}};
		s.equipmentLoads = {load};
		s.stores = {store("misleading-oxygen", "g", 100000, 50000, 0, "feed"), store("return", "g", 100000, 0, 0, "product"),
			store("fuel", "Wh", 100000, 100000, 0, "primary-fuel")};
		core::ThermalReservoirSettings sink; sink.id = "sink"; sink.thermalClass = "warm";
		sink.heatCapacityKJPerK = 1e6; sink.startingTemperatureC = 20; sink.minTemperatureC = 0; sink.maxTemperatureC = 100;
		s.thermalReservoirs = {sink};
		core::GeneratorSettings g; g.id = "gen"; g.fuelStoreId = "fuel"; g.heatSinkId = "sink"; g.thermalClass = "warm";
		g.ratedOutputKW = 10; g.commandedOutputKW = 8; g.efficiency = 0.5; g.maxRampKWPerSecond = 2;
		g.startupPowerKW = 1; g.startupSeconds = 0.2; g.maxSinkTemperatureC = 90;
		s.generators = {g};
		core::World world("Electrical exchanges", 2, 2); world.pauseSimulation(); world.finishBuild();
		auto apply = [&] { std::string why; if (!world.configureStationSystems(s, true, &why)) throw std::runtime_error(why); };
		auto definition = [&](std::string const& type, std::string const& role, std::string const& material = "energy") {
			auto const catalogue = world.getStationSystemsObservationCatalogue();
			auto const found = std::find_if(catalogue.begin(), catalogue.end(), [&](auto const& c) {
				return c.breakdown == "Aggregate" && c.id.starts_with("exchange/" + type + "/")
					&& c.id.find("/" + role + "/" + material + "/") != std::string::npos;
			});
			require(found != catalogue.end(), "electrical exchange not discovered"); return *found;
		};
		auto value = [&](std::string const& type, std::string const& role, std::string const& material = "energy") {
			auto data = world.queryStationSystemsChart(definition(type, role, material).id);
			require(!data.incomplete && !data.series[0].values.empty(), "electrical exchange unavailable");
			return data.series[0].values.back();
		};
		apply();
		require(world.queryStationSystemsChart(definition("Generator", "Generation").id).series[0].values.empty(), "new run fabricated samples");
		run(world, 6); // Starting: separate demand, received crank and heat; no generation.
		auto snap = world.getStationSystemsSnapshot();
		close(value("Generator", "Generation"), snap.power.generators[0].outputKW);
		close(value("Generator", "Startup electricity"), snap.power.generators[0].startupDrawKW);
		close(value("Generator", "Startup heat"), snap.power.generators[0].startupDrawKW);
		require(snap.power.generators[0].startupDrawKW > 0, "fixture did not start generator");
		run(world, 60); snap = world.getStationSystemsSnapshot();
		close(value("Solar array", "Available generation"), snap.power.sources[0].availableKW);
		close(value("Generator", "Generation"), snap.power.generators[0].outputKW);
		require(snap.power.generators[0].outputKW > 0 && snap.power.generators[0].outputKW < 8, "fixture did not exercise ramp");
		close(value("Generator", "Fuel", "primary-fuel"), snap.power.generators[0].fuelDrawKWh * 3600 / core::World::getFixedTimestep());
		close(value("Generator", "Production waste heat"), snap.power.generators[0].wasteHeatKW);
		close(value("Battery", "Charging electricity"), snap.power.batteries[0].lastChargeKW);
		close(value("Battery", "Charge conversion loss"), snap.power.batteries[0].lastChargeKW * 0.2);
		close(value("External equipment load group", "Electrical service"), snap.power.loads[0].suppliedKW);
		close(value("External equipment load group", "Exported heat"), snap.power.loads[0].heatExportedKW);
		close(value("External equipment load group", "Material in 1", "feed"), snap.power.loads[0].materialExchanges[0].completed * 0.001 / core::World::getFixedTimestep());
		close(value("External equipment load group", "Material return 1", "product"), snap.power.loads[0].materialExchanges[1].completed * 0.001 / core::World::getFixedTimestep());
		require(value("Solar array", "Available generation") > value("External equipment load group", "Electrical service"), "surplus generation confused with service");
		auto histories = world.getStationSystemsMetricHistory();
		auto generationDefinition = definition("Generator", "Generation");
		auto const data = world.queryStationSystemsChart(generationDefinition.id);
		require(data.series.size() == 1, "generation invented a requested output");
		for (auto& h : histories) if (h.descriptor.equipmentType == "Generator" && h.descriptor.exchangeRole == "Generation") h.samples.back().actual.reset();
		auto const missing = core::queryStationChart(generationDefinition, histories);
		require(missing.incomplete && std::isnan(missing.series[0].values.back()), "missing generator became zero");
		// Reapply with no generation: battery supports a constrained, lossy cable.
		s.solarArrays.clear(); s.generators.clear();
		auto const cable = std::find_if(snap.connections.begin(), snap.connections.end(), [](auto const& c) {
			return c.key.ownerType == "Equipment" && c.key.ownerId == "load" && c.key.relation == "Electrical supply";
		});
		require(cable != snap.connections.end(), "equipment cable authority absent");
		s.connections[cable->key] = {core::StationPhysicalConnection::Automatic, 4, 0.5};
		apply(); run(world, 6); snap = world.getStationSystemsSnapshot();
		close(value("Battery", "Discharging electricity"), snap.power.batteries[0].lastDischargeKW);
		close(value("Battery", "Discharge conversion loss"), snap.power.batteries[0].lastDischargeKW * (1 / 0.7 - 1));
		close(value("External equipment load group", "Electrical service"), 2);
		close(value("External equipment load group", "Supply cable loss"), 2);
		close(world.queryStationSystemsChart("power").series[1].values.back(), 0);
		require(snap.power.totalSuppliedKW > 0, "battery fixture supplied no service");
		// Fuel cells have distinct H2/O2 reactant properties, never one fuel sum.
		s.connections.clear(); s.equipmentLoads.clear(); s.batteries.clear();
		s.stores = {store("h", "kg", 100, 50, 0), store("o", "kg", 100, 50, 0), store("w", "kg", 100, 0, 0)};
		core::FuelCellSettings cell; cell.id = "cell"; cell.hazardClass = "normal"; cell.hydrogenStoreId = "h"; cell.oxygenStoreId = "o"; cell.waterStoreId = "w";
		cell.thermalClass = "warm"; cell.heatSinkId = "sink"; cell.ratedOutputKW = 10; cell.commandedOutputKW = 5;
		s.fuelCells = {cell}; apply(); run(world, 6); snap = world.getStationSystemsSnapshot();
		close(value("Fuel cell", "Hydrogen", "hydrogen"), snap.fuelCells[0].lastHydrogenKg / core::World::getFixedTimestep());
		close(value("Fuel cell", "Oxygen", "oxygen"), snap.fuelCells[0].lastOxygenKg / core::World::getFixedTimestep());
		close(value("Fuel cell", "Product water", "water"), snap.fuelCells[0].lastWaterKg / core::World::getFixedTimestep());
		close(value("Fuel cell", "Generation"), snap.fuelCells[0].lastOutputKW);
		// Stopped equipment is a measured zero, not missing authority.
		s.fuelCells[0].commandedOutputKW = 0; apply(); run(world, 6);
		close(value("Fuel cell", "Generation"), 0); close(value("Fuel cell", "Hydrogen", "hydrogen"), 0);
		// Expected membership and generic breakdowns cover multiple instances.
		s.fuelCells.clear(); s.stores.clear(); s.solarArrays = {{"sun", 20}};
		load.materialExchanges.clear(); load.electricalDemandKW = 3; s.equipmentLoads = {load};
		load.id = "second"; load.electricalDemandKW = 2; s.equipmentLoads.push_back(load);
		apply(); run(world, 6);
		auto const service = definition("External equipment load group", "Electrical service");
		auto const aggregate = world.queryStationSystemsChart(service.id);
		auto const instances = world.queryStationSystemsChart(service.id + "/instances");
		close(aggregate.series[0].values.back(), 5); close(aggregate.series[1].values.back(), 5);
		require(instances.series.size() == 4 && instances.series[1].stacked && instances.series[2].stacked
			&& !instances.series[3].stacked, "electrical actual/request breakdown population differs");
		close(instances.series[1].values.back() + instances.series[2].values.back(), aggregate.series[0].values.back());
		world.resetSimulation(); run(world, 6);
		close(world.queryStationSystemsChart(service.id).series[0].values.back(), 5);
	}

	void materialObservations(smoke::Context const&)
	{
		core::World world("Materials", 2, 2); world.pauseSimulation(); world.finishBuild();
		auto settings = rig(); std::string diagnostic;
		require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
		world.markSaved();
		auto const waterDefinition = chart(world, "stored-material/water/kg");
		require(waterDefinition.series.size() == 5 && waterDefinition.partialAccounting
			&& waterDefinition.accountingNote.find("1 unclassified") != std::string::npos,
			"stored coverage omitted ownership membership or unclassified inventory");
		require(waterDefinition.label.find("Stored material inventory") == 0,
			"Resource-store-only inventory claimed system coverage");
		run(world);
		require(world.getStationSystemsQueryWork().queries == 0, "recording prepared unselected material views");
		auto const before = world.getStationSystemsSnapshot();
		auto const revision = world.getStationSystemsObservationRevision();
		auto const water = world.queryStationSystemsChart("stored-material/water/kg");
		auto const oxygen = world.queryStationSystemsChart("stored-material/oxygen/kg");
		auto const volume = world.queryStationSystemsChart("stored-material/water/m3");
		auto const unknown = world.queryStationSystemsChart("unclassified/mystery");
		require(water.incomplete && water.series[0].values.size() == 10,
			"unknown inventory was hidden or unselected history was not recorded");
		for (size_t row = 0; row < water.series[0].values.size(); ++row)
		{
			close(water.series[0].values[row], 20); // qualified + unqualified, all hazards, grams converted
			close(oxygen.series[0].values[row], 4); // same unit, different material
			close(volume.series[0].values[row], 1.5); // no guessed mass/volume conversion
			close(unknown.series[0].values[row], 9); // quantity visible, never assigned to water
			double stack = 0;
			for (size_t i = 1; i < water.series.size(); ++i)
			{
				close(water.series[i].lower[row], stack); stack += water.series[i].values[row];
				close(water.series[i].upper[row], stack);
			}
			close(stack, water.series[0].values[row]);
		}
		auto const normal = world.queryStationSystemsChart("stored-usable/water/kg/normal");
		auto const hazardous = world.queryStationSystemsChart("stored-usable/water/kg/hazardous");
		auto const hazard = world.queryStationSystemsChart("stored-material/water/kg/by-hazard");
		auto const aggregate = world.queryStationSystemsChart("stored-material/water/kg/aggregate");
		require(aggregate.series.size() == 1 && hazard.series.size() == 3,
			"generic aggregate or hazard grouping membership incorrect");
		for (size_t row = 0; row < normal.series[0].values.size(); ++row)
		{
			close(normal.series[0].values[row], 11); // reserve remains qualified; unqualified never does
			close(hazardous.series[0].values[row], 2); // never folded into normal supply
			close(hazard.series[1].values[row], 3); close(hazard.series[2].values[row], 17);
			close(hazard.series.back().upper[row], aggregate.series[0].values[row]);
			close(normal.series.back().upper[row], normal.series[0].values[row]);
		}
		require(chart(world, "stored-usable/water/kg/normal").accountingNote.find("NOT discretionary drawability") != std::string::npos,
			"usable inventory implied protected reserves were drawable");
		require(before.stores[0].currentQualified < 10 && before.stores[1].currentQualified > 0,
			"fixture did not exercise internal transfer");
		require(world.getStationSystemsSnapshot() == before && !world.isModified()
			&& world.getStationSystemsObservationRevision() == revision
			&& world.getStationSystemsSettings() == settings, "query mutated applied run or authored identity");
		// Missing expected ownership produces gaps, not a plausible smaller sum.
		auto history = world.getStationSystemsMetricHistory();
		require(std::count_if(history.begin(), history.end(), [](auto const& h) {
			return h.descriptor.equipmentType == "Resource store" && !h.descriptor.constituentId;
		}) == static_cast<int>(2 * settings.stores.size()), "total and qualified observations not registered once per store");
		for (auto const& h : history) if (h.descriptor.equipmentType == "Resource store")
			require(!h.descriptor.requestedSemantics && !h.samples[0].requested, "inventory manufactured a requested value");
		std::erase_if(history, [](auto const& h) { return h.descriptor.instanceId == "grams"; });
		auto const missing = core::queryStationChart(waterDefinition, history);
		require(missing.incomplete && std::isnan(missing.series[0].values[0])
			&& std::isnan(missing.series[1].upper[0]), "missing inventory became zero or a complete stack");
		for (auto const& id : {"stored-material/water/kg/by-hazard", "stored-usable/water/kg/normal"})
		{
			auto const gap = core::queryStationChart(chart(world, id), history);
			require(gap.incomplete && std::isnan(gap.series[0].values[0]) && std::isnan(gap.series.back().upper[0]),
				"grouped/usable missing contribution was concealed by zero filling");
		}
		run(world, 1800);
		auto const coarse = world.queryStationSystemsChart("stored-material/water/kg");
		require(coarse.truncated && coarse.series[0].values.size() <= 256, "material history did not remain bounded");
		for (auto value : coarse.series[0].values) close(value, 20);
		auto const coarseHazard = world.queryStationSystemsChart("stored-material/water/kg/by-hazard");
		auto const coarseUsable = world.queryStationSystemsChart("stored-usable/water/kg/normal");
		for (size_t row = 0; row < coarse.series[0].values.size(); ++row)
		{
			require(coarseHazard.series[0].times[row] == coarse.series[0].times[row]
				&& coarseUsable.series[0].times[row] == coarse.series[0].times[row], "coarsened inventories lost alignment");
			close(coarseHazard.series.back().upper[row], coarse.series[0].values[row]);
			close(coarseUsable.series.back().upper[row], coarseUsable.series[0].values[row]);
			close(coarseUsable.series[0].values[row], 11);
		}
		auto const oldRun = world.getStationSystemsObservationRevision().run;
		settings.stores[5].materialId = "oxygen";
		require(world.configureStationSystems(settings, true), "identity assignment apply failed");
		require(world.getStationSystemsObservationRevision().run != oldRun
			&& world.queryStationSystemsChart("stored-material/oxygen/kg").series[0].times.empty(),
			"identity application joined incompatible histories");
		require(!chart(world, "stored-material/water/kg").partialAccounting
			&& world.queryStationSystemsChart("unclassified/mystery").series.empty(), "assigned identity stayed unclassified");
		run(world, 6); close(world.queryStationSystemsChart("stored-material/oxygen/kg").series[0].values[0], 13);
		// Failed identity application preserves the old catalogue and history.
		auto const appliedRevision = world.getStationSystemsObservationRevision();
		auto invalid = settings; invalid.stores[5].materialId = "Invalid identity";
		require(!world.configureStationSystems(invalid, true) && world.getStationSystemsObservationRevision() == appliedRevision,
			"refused identity application reset the run");
		// Contradictory evidence anywhere in the transfer component excludes
		// every member; no traversal order or explicit identity gets priority.
		settings.stores[1].materialId = "oxygen";
		require(world.configureStationSystems(settings, true), "conflicting observation metadata changed physics validation");
		require(chart(world, "unclassified/oxygen").partialAccounting && chart(world, "unclassified/spare").partialAccounting,
			"explicit transfer identity conflict was silently reinterpreted");
		run(world, 6); close(world.queryStationSystemsChart("stored-material/water/kg").series[0].values[0], 5);
	}

	void fluidInventoryObservations(smoke::Context const&)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		s.stores = {store("raw", "kg", 100, 5, 1, "water")};
		core::AtmosphereSettings air;
		air.id = "shared-name"; air.hazardClass = "normal"; air.volumeM3 = 100;
		air.startingOxygenKg = 20; air.startingNitrogenKg = 80;
		air.startingCarbonDioxideKg = 0.5; air.startingWaterVapourKg = 2;
		air.startingTemperatureC = 25; air.thermalCapacityKJPerK = 10000;
		air.minPressureKPa = 1; air.maxPressureKPa = 200;
		s.atmospheres = {air};
		air.id = "hazard"; air.hazardClass = "hazardous"; air.startingWaterVapourKg = 1;
		s.atmospheres.push_back(air);
		core::ThermalReservoirSettings sink;
		sink.id = "sink"; sink.thermalClass = "moderate"; sink.heatCapacityKJPerK = 10000;
		sink.startingTemperatureC = 10; sink.minTemperatureC = -50; sink.maxTemperatureC = 200;
		s.thermalReservoirs = {sink};
		core::CoolantLoopSettings loop;
		loop.id = "shared-name"; loop.hazardClass = "normal"; loop.reservoirId = "sink";
		loop.coolantCapacityKg = 100; loop.startingCoolantKg = 100; loop.specificHeatKJPerKgK = 4;
		loop.startingTemperatureC = 5; loop.freezeProtectionSetpointC = 0;
		loop.pumpPowerKW = 0.1; loop.ratedFlowKgPerS = 10;
		s.coolantLoops = {loop};
		loop.id = "dry"; loop.startingCoolantKg = 0; s.coolantLoops.push_back(loop);
		core::SolarArraySettings solar; solar.id = "solar"; solar.capacityKW = 10; s.solarArrays = {solar};
		core::WaterCollectorSettings collector;
		collector.id = "collector"; collector.hazardClass = "normal"; collector.atmosphereId = "shared-name";
		collector.coolingLoopId = "shared-name"; collector.thermalClass = "moderate";
		collector.destStoreId = "raw"; collector.maxRateKgPerS = 0.01; collector.targetRelativeHumidity = 0.5;
		s.waterCollectors = {collector};
		core::World world("Fluid owners", 2, 2); world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
		run(world, 600);
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.waterCollectors[0].collectedKg > 0, "fixture did not exchange store/atmosphere water");
		auto const water = world.queryStationSystemsChart("accounted-material/water/kg");
		auto const stored = world.queryStationSystemsChart("stored-material/water/kg");
		require(water.incomplete && chart(world, "accounted-material/water/kg").partialAccounting,
			"pending adapters claimed complete system accounting");
		for (size_t row = 0; row < water.series[0].values.size(); ++row)
		{
			close(water.series[0].values[row], 9);
			close(water.series.back().upper[row], 9);
		}
		require(stored.series[0].values.back() > 6, "stored-only boundary did not track condensation");
		close(water.series[0].values.back(), snap.stores[0].currentQualified + snap.stores[0].currentUnqualified
			+ snap.atmospheres[0].waterVapourKg + snap.atmospheres[1].waterVapourKg);
		close(world.queryStationSystemsChart("accounted-material/oxygen/kg").series[0].values.back(), 40);
		close(world.queryStationSystemsChart("accounted-material/nitrogen/kg").series[0].values.back(), 160);
		close(world.queryStationSystemsChart("accounted-material/carbon-dioxide/kg").series[0].values.back(), 1);
		close(world.queryStationSystemsChart("accounted-material/coolant/kg").series[0].values.back(), 100);
		auto const hazard = world.queryStationSystemsChart("accounted-material/water/kg/by-hazard");
		close(hazard.series[1].values.back(), 1); close(hazard.series[2].values.back(), 8);
		auto const equipment = world.queryStationSystemsChart("accounted-material/water/kg/by-equipment");
		close(equipment.series.back().upper.back(), 9);
		// Compartment stock is not qualified store supply, nor circulating flow.
		close(world.queryStationSystemsChart("stored-usable/water/kg/normal").series[0].values.back(),
			snap.stores[0].currentQualified);
		auto histories = world.getStationSystemsMetricHistory();
		require(std::count_if(histories.begin(), histories.end(), [](auto const& h) {
			return h.descriptor.exchangeRole == "retained-inventory";
		}) == 10, "non-store stock registered twice or omitted");
		for (auto const& h : histories) if (h.descriptor.exchangeRole == "retained-inventory")
			require(!h.descriptor.requestedSemantics && !h.samples[0].requested, "stock invented requested semantics");
		std::erase_if(histories, [](auto const& h) { return h.descriptor.id == "atmosphere/shared-name/water/inventory"; });
		for (auto id : {"accounted-material/water/kg", "accounted-material/water/kg/by-equipment",
			"accounted-material/water/kg/by-hazard"})
		{
			auto const missing = core::queryStationChart(chart(world, id), histories);
			require(missing.incomplete && std::isnan(missing.series[0].values[0])
				&& std::isnan(missing.series.back().upper[0]), "missing fluid inventory became a zero or valid stack");
		}
		auto const revision = world.getStationSystemsObservationRevision();
		world.resetSimulation();
		require(world.getStationSystemsObservationRevision().run != revision.run
			&& world.queryStationSystemsChart("accounted-material/water/kg").series[0].times.empty(),
			"Reset joined fluid inventory runs");
		run(world, 600);
		require(world.getStationSystemsSnapshot() == snap, "fluid observation changed deterministic replay");
	}

	void compoundInventoryObservations(smoke::Context const&)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		s.stores = {store("food", "kg", 100, 8, 2, "food"), store("buffer", "kg", 100, 0, 0),
			store("waste", "g", 10000, 2000, 2000, "organic-waste"),
			store("hazard", "kg", 100, 1, 1, "organic-waste"), store("unknown", "kg", 100, 3, 0)};
		core::ResourceCompositionSettings composition{0.25, 0.1, 0.2, 0.1, 0.1, 0.1, 3600};
		for (size_t i = 0; i < 4; ++i) s.stores[i].composition = composition;
		s.stores[3].hazardClass = "hazardous"; s.stores[0].protectedReserve = 2;
		s.transfers = {{"transfer", "food", "buffer", "kg", 1}};
		// Unqualified waste is retained but not ejectable, so ejection can
		// remove only the qualified half. The loss ledger is not extra stock.
		s.wasteEjections = {{"eject", "waste", "g", 3000, 0.5}};
		core::World world("Compound owners", 2, 2); world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
		run(world, 60);
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.stores[1].currentQualified > 0 && snap.wasteEjections[0].ejectedTotal == 2000,
			"compound fixture did not transfer/eject material");
		double partition = 0;
		for (auto key : core::StationMassConstituents)
		{
			auto const id = std::string("stored-constituent/") + key + "/kg";
			auto const view = world.queryStationSystemsChart(id);
			require(!view.series.empty() && view.incomplete, "constituent coverage claimed complete system accounting");
			double actual = 0, usable = 0;
			for (size_t i = 0; i < 4; ++i)
			{
				auto const& owner = snap.stores[i];
				actual += owner.actualConstituents.at(key);
				if (owner.hazardClass == "normal")
					usable += owner.actualConstituents.at(key) * owner.currentQualified
						/ (owner.currentQualified + owner.currentUnqualified);
			}
			close(view.series[0].values.back(), actual);
			close(view.series.back().upper.back(), actual);
			partition += actual;
			auto const qualified = world.queryStationSystemsChart(std::string("stored-usable-constituent/") + key + "/kg/normal");
			close(qualified.series[0].values.back(), usable);
			close(qualified.series.back().upper.back(), usable);
			auto const hazard = world.queryStationSystemsChart(id + "/by-hazard");
			close(hazard.series.back().upper.back(), actual);
		}
		close(partition, 14); // 10 food + 2 remaining unqualified waste + 2 hazardous waste
		close(world.queryStationSystemsChart("stored-constituent/water/kg").series[0].values.back(), 3.5);
		close(world.queryStationSystemsChart("stored-constituent/nutrientNitrogen/kg").series[0].values.back(), 1.05);
		close(world.queryStationSystemsChart("stored-constituent/atmosphericNitrogen/kg").series[0].values.back(), 2.1);
		close(world.queryStationSystemsChart("stored-constituent/energy/kWh").series[0].values.back(), 10.5);
		close(world.queryStationSystemsChart("stored-material/food/kg").series[0].values.back(), 10);
		close(world.queryStationSystemsChart("accounted-material/organic-waste/kg").series[0].values.back(), 4);
		require(world.queryStationSystemsChart("accounted-material/water/kg").series.empty(),
			"bound moisture was silently added to free-water material");
		auto const definition = chart(world, "stored-constituent/water/kg");
		require(definition.accountingNote.find("retained waste") != std::string::npos
			&& definition.accountingNote.find("not a system total") != std::string::npos,
			"compound boundary labels missing");
		auto history = world.getStationSystemsMetricHistory();
		for (auto const& h : history) if (h.descriptor.constituentId)
			require(!h.descriptor.materialId && !h.descriptor.requestedSemantics && !h.samples[0].requested,
				"constituent manufactured material ownership or demand");
		std::erase_if(history, [](auto const& h) { return h.descriptor.instanceId == "waste"; });
		auto const missing = core::queryStationChart(definition, history);
		require(std::isnan(missing.series[0].values[0]) && std::isnan(missing.series.back().upper[0]),
			"missing compound owner shrank aggregate membership");
		world.resetSimulation(); run(world, 60);
		require(world.getStationSystemsSnapshot() == snap, "compound recording altered deterministic replay");
	}

	void livingInventoryObservations(smoke::Context const&)
	{
		for (int scenario = 0; scenario < 4; ++scenario)
		{
			core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
			core::ResourceCompositionSettings composition;
			composition.nutrientNitrogen = 0.5; composition.phosphorus = 0.1;
			composition.potassium = 0.1; composition.otherMinerals = 0.1;
			composition.energyKJPerKgDry = 3600;
			s.stores = {store("water", "kg", 1000, 100, 0, "water")};
			for (auto id : {"N", "P", "K", "minerals", "food", "waste", "recovery"})
			{
				s.stores.push_back(store(id, "kg", 100, s.stores.size() < 5 ? 10 : 0, 0, "compound"));
				s.stores.back().composition = composition;
			}
			for (size_t i = 1; i <= 4; ++i)
			{
				core::ResourceCompositionSettings pure;
				if (i == 1) pure.nutrientNitrogen = 1;
				if (i == 2) pure.phosphorus = 1;
				if (i == 3) pure.potassium = 1;
				if (i == 4) pure.otherMinerals = 1;
				s.stores[i].composition = pure;
			}
			core::AtmosphereSettings air;
			air.id = "habitat"; air.hazardClass = "normal"; air.volumeM3 = 100;
			air.startingOxygenKg = 40; air.startingNitrogenKg = 80; air.startingCarbonDioxideKg = 5;
			air.startingWaterVapourKg = 1; air.startingTemperatureC = 22;
			air.thermalCapacityKJPerK = 20000; air.minPressureKPa = 1; air.maxPressureKPa = 200;
			s.atmospheres = {air};
			core::SolarArraySettings solar; solar.id = "sun"; solar.capacityKW = 100; s.solarArrays = {solar};
			auto configure = [&](auto& p) {
				p.id = "same-id"; p.atmosphereId = "habitat"; p.waterStoreId = "water";
				p.nitrogenStoreId = "N"; p.phosphorusStoreId = "P";
				p.potassiumStoreId = "K"; p.mineralsStoreId = "minerals";
				p.composition = composition; p.initialBiomassKg = 1;
				p.lampRatedKW = 3.6; p.circulationPowerKW = 0.1; p.growthYieldKgPerKWh = 0.5;
				p.illuminationProfile.constantLevel = scenario >= 2 ? 0 : 1;
				p.respirationPerHour = scenario >= 2 ? 600 : 0;
				p.foodStoreId = "food"; p.wasteStoreId = scenario == 3 ? "" : "waste";
				p.harvestTriggerKg = scenario == 1 ? 1 : 0; p.harvestFraction = 0.5;
			};
			core::CropPopulationSettings crop; configure(crop); crop.growingAreaM2 = 10;
			core::AlgaeCultureSettings algae; configure(algae); algae.cultureVolumeL = 10000;
			algae.recoveryStoreId = "recovery";
			s.cropPopulations = {crop}; s.algaeCultures = {algae};
			core::World world("Living inventories", 2, 2); world.pauseSimulation(); world.finishBuild();
			std::string diagnostic;
			if (!world.configureStationSystems(s, false, &diagnostic)) throw std::runtime_error(diagnostic);
			run(world, 300);
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0]; auto const& a = snap.algaeCultures[0];
			auto value = [&](std::string const& id) {
				auto const q = world.queryStationSystemsChart(id);
				require(!q.series.empty() && !q.series[0].values.empty(), "living chart missing");
				return q.series[0].values.back();
			};
			std::string const biomass = "accounted-material/living-biomass/kg";
			close(value(biomass), c.livingBiomassKg + a.livingBiomassKg);
			close(c.livingConstituents.organicKg + c.livingConstituents.nutrientNitrogenKg
				+ c.livingConstituents.phosphorusKg + c.livingConstituents.potassiumKg
				+ c.livingConstituents.mineralsKg, c.livingBiomassKg);
			close(a.livingConstituents.organicKg + a.livingConstituents.nutrientNitrogenKg
				+ a.livingConstituents.phosphorusKg + a.livingConstituents.potassiumKg
				+ a.livingConstituents.mineralsKg, a.livingBiomassKg);
			// Uptake and harvest only move bound nitrogen between owners;
			// uncontained death is the sole loss from this closed boundary.
			close(value("accounted-constituent/nutrientNitrogen/kg"), scenario == 3 ? 10 : 11);
			close(value(biomass + "/aggregate"), value(biomass));
			close(value(biomass + "/by-equipment"), value(biomass));
			auto const stack = world.queryStationSystemsChart(biomass + "/by-equipment");
			close(stack.series.back().upper.back(), value(biomass));
			core::StationBiologicalConstituents living = c.livingConstituents;
			living += a.livingConstituents;
			double const masses[] = {living.organicKg, living.nutrientNitrogenKg, living.phosphorusKg,
				living.potassiumKg, living.mineralsKg};
			char const* keys[] = {"dryOrganics", "nutrientNitrogen", "phosphorus", "potassium", "otherMinerals"};
			for (size_t i = 0; i < 5; ++i)
			{
				auto id = std::string("accounted-constituent/") + keys[i] + "/kg";
				double expected = masses[i];
				for (auto const& owner : snap.stores) if (!owner.actualConstituents.empty())
					expected += owner.actualConstituents.at(keys[i]);
				close(value(id), expected); close(value(id + "/aggregate"), expected);
				close(world.queryStationSystemsChart(id + "/by-equipment").series.back().upper.back(), expected);
				close(value(id), value(std::string("stored-constituent/") + keys[i] + "/kg") + masses[i]);
			}
			close(value("accounted-constituent/energy/kWh"), value("stored-constituent/energy/kWh")
				+ (c.livingChemicalEnergyKJ + a.livingChemicalEnergyKJ) / 3600);
			// No inferred culture liquid or biological moisture; free water and
			// compound water remain their existing, disjoint boundaries.
			close(value("accounted-constituent/water/kg"), 0);
			for (auto const& h : world.getStationSystemsMetricHistory())
				if (h.descriptor.equipmentType == "Crop population" || h.descriptor.equipmentType == "Algae culture")
					require(h.descriptor.materialId == "living-biomass" || h.descriptor.constituentId,
						"biology invented a water or capacity inventory");
			if (scenario == 0) require(c.livingBiomassKg > 1 && a.livingBiomassKg > 1, "biology did not grow");
			if (scenario == 1) require(c.harvestedKg > 0 && a.harvestedKg > 0, "biology did not harvest");
			if (scenario >= 2) require(c.dead && a.dead, "biology did not starve");
			auto const definition = chart(world, "accounted-constituent/nutrientNitrogen/kg");
			require(definition.partialAccounting && definition.accountingNote.find("biological water") != std::string::npos,
				"biological coverage not declared");
			auto history = world.getStationSystemsMetricHistory();
			std::erase_if(history, [](auto const& h) { return h.descriptor.equipmentType == "Algae culture"; });
			auto const missing = core::queryStationChart(definition, history);
			require(std::isnan(missing.series[0].values.back()) && std::isnan(missing.series.back().upper.back()),
				"unavailable biological owner silently shrank membership");
			world.resetSimulation(); run(world, 300);
			require(world.getStationSystemsSnapshot() == snap, "living observation changed deterministic Reset");
		}
	}

	void materialCanonicalUnits(smoke::Context const&)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		for (auto unit : core::StationStoreUnits)
			s.stores.push_back(store(unit, unit, 10000, 1, 1, "test-material"));
		core::World world("Canonical units", 2, 2); world.pauseSimulation(); world.finishBuild();
		require(world.configureStationSystems(s, false), "canonical unit fixture refused"); run(world, 6);
		close(world.queryStationSystemsChart("stored-material/test-material/kg").series[0].values[0], 2.002);
		close(world.queryStationSystemsChart("stored-material/test-material/m3").series[0].values[0], 2.002002);
		close(world.queryStationSystemsChart("stored-material/test-material/kWh").series[0].values[0],
			2 + 0.002 + 2.0 / 3600 + 2.0 / 3.6);
		close(world.queryStationSystemsChart("stored-material/test-material/mol").series[0].values[0], 2);
		auto const catalogue = world.getStationSystemsObservationCatalogue();
		require(std::count_if(catalogue.begin(), catalogue.end(), [](auto const& c) {
			return c.id.starts_with("stored-material/") && c.id == c.propertyId;
		}) == 4, "compatible units were split or incompatible physical quantities were merged");
	}

	void materialRelationshipObservations(smoke::Context const&)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		s.stores = {store("not-water", "kg", 100, 10, 2), store("not-oxygen", "kg", 100, 3, 1),
			store("not-hydrogen", "kg", 100, 1, 1), store("water", "kg", 100, 2, 2)};
		core::ThermalReservoirSettings sink;
		sink.id = "sink"; sink.thermalClass = "low"; sink.heatCapacityKJPerK = 10000;
		sink.startingTemperatureC = 10; sink.minTemperatureC = -50; sink.maxTemperatureC = 200;
		s.thermalReservoirs = {sink};
		core::ElectrolyzerSettings e;
		e.id = "splitter"; e.hazardClass = "normal";
		e.waterStoreId = "not-water"; e.oxygenStoreId = "not-oxygen"; e.hydrogenStoreId = "not-hydrogen";
		e.thermalClass = "low"; e.heatSinkId = "sink"; e.specificEnergyKWhPerKgWater = 4.5;
		e.maxSinkTemperatureC = 60;
		s.electrolyzers = {e}; // known inactive process still supplies validated identity evidence
		core::World world("Relationship identities", 2, 2); world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
		run(world, 6);
		close(world.queryStationSystemsChart("stored-material/water/kg").series[0].values[0], 12);
		close(world.queryStationSystemsChart("stored-material/oxygen/kg").series[0].values[0], 4);
		close(world.queryStationSystemsChart("stored-material/hydrogen/kg").series[0].values[0], 2);
		close(world.queryStationSystemsChart("unclassified/water").series[0].values[0], 4);
		require(world.getStationSystemsSettings() == s, "legacy resolution rewrote authored identity");
		// Explicit conflicts with intrinsic evidence are not reinterpreted either.
		s.stores[1].materialId = "argon";
		require(world.configureStationSystems(s, true), "explicit conflict metadata rejected by physics");
		require(chart(world, "unclassified/not-oxygen").partialAccounting
			&& world.queryStationSystemsChart("stored-material/argon/kg").series.empty()
			&& world.queryStationSystemsChart("stored-material/oxygen/kg").series.empty(), "intrinsic conflict guessed an identity");
		// Opposing intrinsic evidence connected through a valid transfer is ambiguous.
		s.stores[1].materialId.clear(); s.transfers = {{"ambiguous", "not-oxygen", "not-hydrogen", "kg", 1}};
		require(world.configureStationSystems(s, true), "ambiguous relationship configuration refused");
		require(chart(world, "unclassified/not-oxygen").partialAccounting
			&& chart(world, "unclassified/not-hydrogen").partialAccounting, "contradictory relationships chose one material");
	}
}

void registerMaterialObservationChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"exchangeObservations", exchangeObservations});
	checks.push_back({"crewAtmosphereExchangeObservations", crewAtmosphereExchangeObservations});
	checks.push_back({"waterGasExchangeObservations", waterGasExchangeObservations});
	checks.push_back({"carbonExchangeObservations", carbonExchangeObservations});
	checks.push_back({"cropExchangeObservations", cropExchangeObservations});
	checks.push_back({"scheduledExchangeObservations", scheduledExchangeObservations});
	checks.push_back({"algaeExchangeObservations", algaeExchangeObservations});
	checks.push_back({"microbialExchangeObservations", microbialExchangeObservations});
	checks.push_back({"thermalExchangeObservations", thermalExchangeObservations});
	checks.push_back({"electricalExchangeObservations", electricalExchangeObservations});
	checks.push_back({"materialObservations", materialObservations});
	checks.push_back({"fluidInventoryObservations", fluidInventoryObservations});
	checks.push_back({"compoundInventoryObservations", compoundInventoryObservations});
	checks.push_back({"livingInventoryObservations", livingInventoryObservations});
	checks.push_back({"materialCanonicalUnits", materialCanonicalUnits});
	checks.push_back({"materialRelationshipObservations", materialRelationshipObservations});
}
