#pragma once

#include "core/StationSystems.h"

namespace core
{
	// The shared role catalogue for persistence/migration and authoring validation.
	// A producer has busIds; a consumer has supplyBusIds. Generators implement
	// both roles because cranking is an electrical consumer, too.
	template<class Settings, class Visitor>
	void forEachStationPowerNode(Settings& settings, Visitor&& visit)
	{
		auto entries = [&](auto& collection, char const* type) { for (auto& entry : collection) visit(type, entry); };
		entries(settings.solarArrays, "Solar");
		entries(settings.generators, "Generator");
		entries(settings.fuelCells, "Fuel cell");
		entries(settings.heatEngines, "Heat engine");
		entries(settings.batteries, "Battery");
		entries(settings.equipmentLoads, "Equipment");
		entries(settings.coolantLoops, "Coolant loop");
		entries(settings.chillers, "Chiller");
		entries(settings.waterTreatments, "Water treatment");
		entries(settings.electrolyzers, "Electrolyzer");
		entries(settings.co2Captures, "CO2 capture");
		entries(settings.sabatierReactors, "Sabatier");
		entries(settings.methaneProcessors, "Methane processor");
		entries(settings.microbialProcessors, "Microbial processor");
		entries(settings.cropPopulations, "Crops");
		entries(settings.algaeCultures, "Algae");
	}

	template<class Settings, class Visitor>
	void forEachStationPowerMember(Settings& settings, Visitor&& visit)
	{
		forEachStationPowerNode(settings, [&](auto, auto& entry) { visit(entry); });
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
