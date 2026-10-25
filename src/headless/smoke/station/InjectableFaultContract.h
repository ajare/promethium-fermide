#pragma once

#include "Smoke.h"
#include "core/StationSystemsRuntime.h"

namespace smoke::station
{
// Exercise the public catalogue/runtime contract on a fresh machine for each
// advertised mode, so another active Fault cannot mask an unsupported mode.
inline void requireInjectableFaultContract(core::StationSystemsSettings const& settings)
{
	auto check = [&](auto const& entries) {
		for (auto const& entry : entries)
			for (auto const& mode : core::stationInjectableFaultModes(settings, entry.id))
			{
				core::StationSystemsRuntime run(settings, 0.25f);
				std::string diagnostic;
				auto const accepted = run.injectFault(entry.id, mode,
					{core::FaultCause::Kind::Editor, "catalogue contract"}, &diagnostic);
				require(accepted, (entry.id + "/" + mode + ": " + diagnostic).c_str());
			}
	};
	check(settings.coolantLoops); check(settings.chillers); check(settings.radiators);
	check(settings.heatEngines); check(settings.generators); check(settings.solarArrays);
	check(settings.batteries); check(settings.atmospheres); check(settings.airlockCycles);
	check(settings.waterTreatments); check(settings.sabatierReactors); check(settings.methaneProcessors);
}
}
