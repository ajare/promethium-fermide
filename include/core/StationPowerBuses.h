#pragma once

#include "core/StationSystems.h"

namespace core
{
	// The shared role catalogue for persistence/migration and authoring validation.
	// A producer has busIds; a consumer has supplyBusIds. Generators implement
	// both roles because cranking is an electrical consumer, too.
	template<class Settings, class Visitor>
	void forEachStationPowerMember(Settings& settings, Visitor&& visit)
	{
		auto entries = [&](auto& collection) { for (auto& entry : collection) visit(entry); };
		entries(settings.solarArrays);
		entries(settings.generators);
		entries(settings.fuelCells);
		entries(settings.heatEngines);
		entries(settings.batteries);
		entries(settings.equipmentLoads);
		entries(settings.coolantLoops);
		entries(settings.chillers);
		entries(settings.waterTreatments);
		entries(settings.electrolyzers);
		entries(settings.co2Captures);
		entries(settings.sabatierReactors);
		entries(settings.methaneProcessors);
		entries(settings.microbialProcessors);
		entries(settings.cropPopulations);
		entries(settings.algaeCultures);
	}

	// Default-only configurations keep their historical feature floor and wire
	// shape. Explicit non-default topology requires schema 124. This is shared
	// by World, standalone Station documents and the settings map writer.
	inline bool stationHasPowerBusAuthoring(StationSystemsSettings const& settings)
	{
		bool authored = settings.powerBuses != std::vector<PowerBusSettings>{PowerBusSettings{}};
		forEachStationPowerMember(settings, [&](auto const& entry) {
			if constexpr (requires { entry.busIds; })
				authored |= entry.busIds != std::vector<std::string>{"main"};
			if constexpr (requires { entry.supplyBusIds; })
				authored |= entry.supplyBusIds != std::vector<std::string>{"main"}
					|| entry.supplyMode != StationSupplyMode::Single
					|| entry.transferReturnDwellSeconds != 30.0;
		});
		return authored;
	}
}
