#include "StationCoreChecks.h"
#include "core/StationSystemsRuntime.h"
#include "core/StationConnections.h"
#include <cmath>
#include <stdexcept>

namespace
{
	constexpr float quantum = 1.0f / 60.0f;
	void require(bool ok, char const* message) { if (!ok) throw std::runtime_error(message); }
	void near(double actual, double expected, char const* message)
	{
		if (std::abs(actual - expected) > 1e-10) throw std::runtime_error(message);
	}

	void eventSplit(smoke::Context const&)
	{
		core::ScopedStationStepRatioForTesting explicitClock(std::nullopt);
		for (uint32_t ratio : {1u, 6u, 30u})
		{
			core::StationSystemsSettings s;
			s.enabled = true; s.systemsStepWorldTicks = ratio;
			s.stores = {{"tank", "kg", 100, 0, 0}, {"sink", "kg", 100, 0, 0}};
			s.deliveries = {{"pulse", "tank", "kg", 1, 0.005, 0.025}};
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "tank"; pump.destinationId = "sink";
			pump.unit = "kg"; pump.ratePerSecond = 1;
			s.transfers = {pump};
			s.wasteEjections = {{"drain", "sink", "kg", 0.001, 0.015, 0.05}};
			core::StationSystemsRuntime runtime(s, quantum);
			// Check every coarse boundary, comparing against the analytic continuous
			// solution, not against a count of calls to the drive.
			for (double seconds : {30.0 * quantum, 60.0 * quantum})
			{
				require(runtime.advanceTo(seconds), "Event-split drive failed");
				auto const snap = runtime.snapshot();
				auto const delivered = snap.deliveries[0].acceptedTotal;
				auto const ejected = snap.wasteEjections[0].ejectedTotal;
				near(snap.stores[1].currentQualified, seconds - 0.005 - ejected,
					"Delivery was integrated before its exact occurrence time");
				near(snap.stores[1].lastTickIn, snap.transfers[0].lastCompleted,
					"Published transfer amount omitted earlier sub-intervals");
				near(snap.stores[0].currentQualified + snap.stores[1].currentQualified + ejected,
					delivered, "Split-step material boundary did not conserve mass");
				near(snap.deliveries[0].lastOccurrenceSeconds, 0.48 + (seconds > 0.75 ? 0.5 : 0),
					"Delivery recurrence shifted with step ratio");
			}
			// Ejection changes the rates in force too: the pump can draw only
			// until the exact drain instant, not for a whole enclosing step.
			s.deliveries.clear();
			s.stores[0].startingQualified = 1;
			s.wasteEjections = {{"empty-source", "tank", "kg", 1, 0.007}};
			core::StationSystemsRuntime draining(s, quantum);
			require(draining.advanceTo(30.0 * quantum), "Ejection split drive failed");
			auto const drained = draining.snapshot();
			near(drained.stores[1].currentQualified, 0.007, "Ejection shifted continuous draw time");
			near(drained.wasteEjections[0].ejectedTotal, 0.993, "Ejection lost the pre-event draw");
		}
	}

	void saturationCrossing(smoke::Context const&)
	{
		core::ScopedStationStepRatioForTesting explicitClock(std::nullopt);
		// All crossings lie strictly inside even the finest step, not on
		// convenient tick boundaries. Read only committed public accounts.
		for (bool draining : {false, true})
		for (int constraint : {0, 1, 2, 3})
		{
			std::vector<std::string> reference;
			for (uint32_t ratio : {1u, 6u, 30u})
			{
				core::StationSystemsSettings s;
				s.enabled = true; s.systemsStepWorldTicks = ratio;
				s.stores = {{"source", "kg", 10, 5, 0}, {"sink", "kg", 10, 0, 0}};
				s.stores[0].protectedReserve = 0.7;
				core::ResourceTransferSettings pump;
				pump.id = "pump"; pump.sourceId = "source"; pump.destinationId = "sink";
				pump.unit = "kg"; pump.ratePerSecond = 3;
				s.transfers = {pump};
				for (auto const& link : core::station_connections::build(s).links)
					if (link.designKey.ownerType == "Transfer")
					{
						auto const source = link.designKey.relation == "Source";
						s.connections[link.designKey] = {core::StationPhysicalConnection::Automatic,
							source && constraint == 1 ? 2.0 : (!source && constraint == 2 ? 2.0 : 20.0),
							source ? 0.5 : (constraint == 3 ? 0.0 : 0.8)};
					}
				auto const deliveredRate = constraint == 1 ? 0.8 : (constraint == 2 ? 1.6 : (constraint == 3 ? 0.0 : 3.0));
				auto const drawnRate = constraint == 3 ? 6.0 : deliveredRate / 0.4;
				auto const crossing = 0.137;
				auto const delivered = deliveredRate * crossing;
				if (constraint == 3 && !draining) continue; // all-loss flow cannot fill its receiver
				if (draining) s.stores[0].startingQualified = 0.7 + drawnRate * crossing;
				else
				{
					s.stores[1].startingUnqualified = 0.1;
					s.stores[1].capacity = 0.1 + delivered;
				}
				core::StationSystemsRuntime runtime(s, quantum);
				auto const step = ratio * static_cast<double>(quantum);
				for (double seconds = step; seconds <= 1.0 + step; seconds += step)
				{
					require(runtime.advanceTo(seconds), "Saturation drive failed");
					auto const snap = runtime.snapshot();
					auto const activeSeconds = std::min(snap.elapsedSeconds, crossing);
					near(snap.stores[1].currentQualified, deliveredRate * activeSeconds, "Capacity crossing delivered wrong amount");
					near(snap.stores[0].currentQualified, s.stores[0].startingQualified - drawnRate * activeSeconds,
						"Reserve crossing drew wrong amount");
					double loss = 0;
					for (auto const& connection : snap.connections)
					{
						near(connection.inputTotal, connection.deliveredTotal + connection.lostTotal,
							"Crossing connection did not conserve material");
						loss += connection.lostTotal;
					}
					near(snap.stores[0].currentQualified + snap.stores[1].currentQualified + loss,
						s.stores[0].startingQualified, "Saturation material boundary did not close");
					if (snap.elapsedSeconds > crossing)
						require(snap.transfers[0].status == core::StationTransferStatus::Rejected,
							"Saturated transfer did not stall");
				}
				std::vector<std::string> transitions;
				for (auto const& event : runtime.events())
					if (event.category == "transfer") transitions.push_back(event.message);
				if (transitions.size() != (constraint == 3 ? 1u : 2u))
				{
					std::string message = "Saturation transition count: ratio=" + std::to_string(ratio)
						+ " constraint=" + std::to_string(constraint) + " draining=" + std::to_string(draining);
					for (auto const& transition : transitions) message += "\n" + transition;
					throw std::runtime_error(message);
				}
				if (reference.empty()) reference = transitions;
				else require(transitions == reference, "Saturation transition order changed with ratio");
			}
		}
	}

	void eventOrder(smoke::Context const&)
	{
		core::ScopedStationStepRatioForTesting explicitClock(std::nullopt);
		for (uint32_t ratio : {1u, 6u, 30u})
		{
			core::StationSystemsSettings s;
			s.enabled = true; s.systemsStepWorldTicks = ratio;
			s.stores = {{"tank", "kg", 1, 0, 0}, {"sink", "kg", 1, 0, 0}};
			// Reverse priority declaration, then tie declaration. All at one
			// instant: delivery -> scheduled transfer -> ejection remains causal.
			s.deliveries = {{"low", "tank", "kg", 1, 0.005, 0, true, 0},
				{"high", "tank", "kg", 0.5, 0.005, 0, true, 1},
				{"tie", "tank", "kg", 0.5, 0.005, 0, true, 0}};
			core::ResourceTransferSettings transfer;
			transfer.id = "batch"; transfer.sourceId = "tank"; transfer.destinationId = "sink";
			transfer.unit = "kg"; transfer.mode = core::ResourceTransferMode::Scheduled;
			transfer.quantity = 1; transfer.firstOccurrenceSeconds = 0.005;
			s.transfers = {transfer};
			s.wasteEjections = {{"eject", "sink", "kg", 1, 0.005}};
			core::StationSystemsRuntime runtime(s, quantum);
			require(runtime.advanceTo(30.0 * quantum), "Coincident event drive failed");
			auto const snap = runtime.snapshot();
			near(snap.deliveries[0].acceptedTotal, 0.5, "Priority order changed");
			near(snap.deliveries[1].acceptedTotal, 0.5, "High priority did not land first");
			near(snap.deliveries[2].acceptedTotal, 0, "Declaration tie order changed");
			near(snap.transfers[0].cumulativeMoved, 1, "Transfer did not follow deliveries");
			near(snap.wasteEjections[0].ejectedTotal, 1, "Ejection did not follow transfer");
			near(snap.stores[0].currentQualified + snap.stores[1].currentQualified, 0,
				"Coincident events created inventory");
			s.transfers.clear(); s.wasteEjections.clear();
			s.deliveries = {{"later-high", "tank", "kg", 1, 0.009, 0, true, 100},
				{"earlier-low", "tank", "kg", 1, 0.005, 0, true, 0}};
			core::StationSystemsRuntime chronological(s, quantum);
			require(chronological.advanceTo(30.0 * quantum), "Chronological drive failed");
			auto const ordered = chronological.snapshot();
			near(ordered.deliveries[0].acceptedTotal, 0, "Priority overrode occurrence-time ordering");
			near(ordered.deliveries[1].acceptedTotal, 1, "Earlier low-priority delivery did not land first");
		}
	}

	void eventRollback(smoke::Context const&)
	{
		core::ScopedStationStepRatioForTesting explicitClock(std::nullopt);
		core::StationSystemsSettings s;
		s.enabled = true; s.systemsStepWorldTicks = 30;
		s.stores = {{"tank", "kg", 1e308, 0, 0}, {"sink", "kg", 100, 0, 0}};
		s.deliveries = {{"huge", "tank", "kg", 1e308, 0.005, 0.025}};
		s.wasteEjections = {{"drain", "tank", "kg", 1e308, 0.015}};
		core::ResourceTransferSettings pump;
		pump.id = "pump"; pump.sourceId = "tank"; pump.destinationId = "sink";
		pump.unit = "kg"; pump.ratePerSecond = 1; s.transfers = {pump};
		core::StationSystemsRuntime runtime(s, quantum);
		auto const before = runtime.snapshot();
		auto const events = runtime.events();
		require(!runtime.advanceTo(30.0 * quantum), "Later split overflow was committed");
		auto const after = runtime.snapshot();
		require(!after.healthy && after.elapsedSeconds == before.elapsedSeconds,
			"Failed split advanced the Station clock");
		require(after.stores == before.stores && after.transfers == before.transfers
			&& after.deliveries == before.deliveries && after.wasteEjections == before.wasteEjections,
			"Failed split published earlier sub-interval inventory or event accounting");
		require(runtime.events() == events, "Failed split changed committed event history");
	}

	void maintenanceSplit(smoke::Context const&)
	{
		core::ScopedStationStepRatioForTesting explicitClock(std::nullopt);
		for (uint32_t ratio : {1u, 6u, 30u})
		{
			core::StationSystemsSettings s;
			s.enabled = true; s.systemsStepWorldTicks = ratio;
			s.stores = {{"parts", "kg", 10, 1, 0}, {"waste", "kg", 10, 0, 0}};
			// A dormant chiller is sufficient to exercise the real installation
			// isolation and material reservation contract, without World machinery.
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "moderate"; sink.heatCapacityKJPerK = 1000;
			auto cold = sink; cold.id = "cold";
			s.thermalReservoirs = {sink, cold};
			core::CoolantLoopSettings loop;
			loop.id = "loop"; loop.reservoirId = "cold";
			loop.coolantCapacityKg = 1; loop.specificHeatKJPerKgK = 4;
			s.coolantLoops = {loop};
			core::ChillerSettings chiller;
			chiller.id = "machine"; chiller.thermalClass = "moderate";
			chiller.coldLoopId = "loop"; chiller.condenserReservoirId = "sink";
			s.chillers = {chiller};
			core::MaintenanceJobSettings job;
			job.id = "service"; job.installationId = "machine";
			job.firstOccurrenceSeconds = 0.007; job.durationSeconds = 0.031;
			job.recurrenceSeconds = 0.075;
			job.requirements = {{"parts", 0.1, true}}; job.wasteStoreId = "waste";
			s.maintenanceJobs = {job};
			core::StationSystemsRuntime runtime(s, quantum);
			require(runtime.advanceTo(30.0 * quantum), "Maintenance split drive failed");
			auto const snap = runtime.snapshot();
			auto const& result = snap.maintenanceJobs[0];
			near(result.lastStartSeconds, 0.457, "Maintenance start shifted with ratio");
			near(result.lastCompleteSeconds, 0.488, "Maintenance completion shifted with ratio");
			near(snap.stores[0].currentQualified, 0.3, "Maintenance consumed wrong material");
			near(snap.stores[1].currentQualified, 0.7, "Maintenance waste was not conserved");
			// A completion exactly on a full-step endpoint must publish the
			// restored installation immediately, without one more integration.
			s.maintenanceJobs[0].firstOccurrenceSeconds = 0;
			s.maintenanceJobs[0].durationSeconds = 30.0 * quantum;
			s.maintenanceJobs[0].recurrenceSeconds = 0;
			core::StationSystemsRuntime endpoint(s, quantum);
			require(endpoint.advanceTo(30.0 * quantum), "Endpoint maintenance drive failed");
			auto const completed = endpoint.snapshot();
			require(completed.maintenanceJobs[0].phase == core::StationMaintenancePhase::Completed
				&& !completed.maintenanceJobs[0].isolated
				&& completed.chillers[0].availabilityFactor == 1,
				"Endpoint completion left installation unavailable");
		}
	}
}

void registerStationEventSplitChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"saturationCrossing", saturationCrossing});
	checks.push_back({"eventSplit", eventSplit});
	checks.push_back({"eventOrder", eventOrder});
	checks.push_back({"eventRollback", eventRollback});
	checks.push_back({"maintenanceSplit", maintenanceSplit});
}
