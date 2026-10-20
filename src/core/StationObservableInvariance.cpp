#include "core/StationObservableInvariance.h"

#include <array>

namespace core
{
	std::span<StationObservableInvariance const> stationObservableInvariances()
	{
		// Keep this list alongside StationSystemsSnapshot. A newly published
		// snapshot field must be classified here before it can claim a timestep
		// guarantee in a released build.
		static constexpr std::array<StationObservableInvariance, 40> entries{{
			{"enabled", StationObservableInvarianceClass::Exact, 0, 0, 0, 0, 600, "authored setting", "station/invariance"},
			{"healthy", StationObservableInvarianceClass::Exact, 0, 0, 0, 0, 600, "transactional step commit", "station/invariance"},
			{"worldTick", StationObservableInvarianceClass::Tolerance, 0, 0, 1, 0, 600, "counts Station steps rather than elapsed state", "station/invariance"},
			{"elapsedSeconds", StationObservableInvarianceClass::Exact, 1e-12, 0, 0, 0, 600, "count-derived Station clock", "station/invariance"},
			{"completedTicks", StationObservableInvarianceClass::Tolerance, 0, 0, 1, 0, 600, "per-step lifecycle counters", "station/invariance"},
			{"stores", StationObservableInvarianceClass::Exact, 1e-10, 0, 0, 0, 600, "event splitting and crossing-time resolution", "station/invariance"},
			{"transfers", StationObservableInvarianceClass::Exact, 1e-10, 0, 0, 0, 600, "rate integration and crossing-time resolution", "station/invariance"},
			{"connections", StationObservableInvarianceClass::Exact, 1e-10, 0, 0, 0, 600, "rate integration and conservation ledger", "station/invariance"},
			{"conversions", StationObservableInvarianceClass::Tolerance, 1e-8, 1e-8, 1, 0, 600, "coupled allocator", "station/invariance"},
			{"balances", StationObservableInvarianceClass::Exact, 1e-10, 0, 0, 0, 600, "conservation ledger", "station/invariance"},
			{"constituentBalances", StationObservableInvarianceClass::Exact, 1e-10, 0, 0, 0, 600, "conservation ledger", "station/invariance"},
			{"deliveries", StationObservableInvarianceClass::Exact, 1e-10, 0, 0, 0, 600, "exact scheduled-event split", "station/invariance"},
			{"wasteEjections", StationObservableInvarianceClass::Exact, 1e-10, 0, 0, 0, 600, "exact scheduled-event split", "station/invariance"},
			{"outages", StationObservableInvarianceClass::Exact, 1e-10, 0, 0, 0, 600, "exact scheduled-event split", "station/invariance"},
			{"maintenanceJobs", StationObservableInvarianceClass::Exact, 1e-10, 0, 0, 0, 600, "exact scheduled-event split", "station/invariance"},
			{"power", StationObservableInvarianceClass::Tolerance, 1e-8, 1e-8, 1, 0, 600, "coupled electrical allocation", "station/invariance"},
			{"thermal", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "explicit thermal integration", "station/invariance"},
			{"coolantLoops", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "explicit thermal integration", "station/invariance"},
			{"chillers", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "coupled thermal allocation", "station/invariance"},
			{"thermalBuffers", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "coupled thermal allocation", "station/invariance"},
			{"atmospheres", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "coupled atmosphere integration", "station/invariance"},
			{"airlockCycles", StationObservableInvarianceClass::Exact, 1e-10, 0, 0, 0, 600, "exact scheduled-event split", "station/invariance"},
			{"crews", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "coupled atmosphere allocation", "station/invariance"},
			{"waterCollectors", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "coupled water integration", "station/invariance"},
			{"waterTreatments", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "coupled water allocation", "station/invariance"},
			{"electrolyzers", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "coupled atmosphere allocation", "station/invariance"},
			{"co2Captures", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "coupled atmosphere allocation", "station/invariance"},
			{"sabatierReactors", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "coupled process allocation", "station/invariance"},
			{"methaneProcessors", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "coupled process allocation", "station/invariance"},
			{"microbialProcessors", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "coupled process allocation", "station/invariance"},
			{"cropPopulations", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "biological integration", "station/invariance"},
			{"algaeCultures", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "biological integration", "station/invariance"},
			{"fuelCells", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "coupled process allocation", "station/invariance"},
			{"hasNextDelivery", StationObservableInvarianceClass::Exact, 0, 0, 0, 0, 600, "schedule ordering", "station/invariance"},
			{"nextDeliverySeconds", StationObservableInvarianceClass::Exact, 1e-12, 0, 0, 0, 600, "schedule ordering", "station/invariance"},
			{"hasNextWasteEjection", StationObservableInvarianceClass::Exact, 0, 0, 0, 0, 600, "schedule ordering", "station/invariance"},
			{"nextWasteEjectionSeconds", StationObservableInvarianceClass::Exact, 1e-12, 0, 0, 0, 600, "schedule ordering", "station/invariance"},
			{"endurance", StationObservableInvarianceClass::Tolerance, 1e-7, 1e-7, 1, 0, 600, "observation-window calculation", "station/invariance"},
			{"forecast", StationObservableInvarianceClass::Exact, 1e-10, 0, 0, 0, 600, "declared-profile calculation", "station/invariance"},
			{"balanceTolerance", StationObservableInvarianceClass::Exact, 0, 0, 0, 0, 600, "published constant", "station/invariance"},
		}};
		return entries;
	}
}
