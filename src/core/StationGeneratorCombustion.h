#pragma once
#include "core/StationConnectionLedger.h"

namespace core::station_detail
{
// The event predictor and the execution ramp must agree on the attainable
// endpoint. Otherwise a duct-limited machine repeatedly predicts a tiny ramp
// toward power it cannot reach, splitting one Station step millions of times.
inline double combustionOutputCeiling(GeneratorSettings const& generator,
	double oxygenKg, double mixtureKg, double efficiency,
	StationConnectionLedger const& connections)
{
	if (!generator.methaneCombustion) return std::numeric_limits<double>::infinity();
	if (oxygenKg <= 0.0 || mixtureKg <= 0.0) return 0.0;
	auto const airPerFuel = 4.0 * mixtureKg / oxygenKg;
	auto const inlet = connections.port("Generator", generator.id, "Combustion air");
	auto const outlet = connections.port("Generator", generator.id, "Combustion exhaust");
	auto const inletRate = connections.efficiency(inlet) > 0.0
		? connections.remaining(inlet) * connections.efficiency(inlet) / airPerFuel : 0.0;
	auto const fuelRate = std::min(inletRate, connections.remaining(outlet) / (airPerFuel + 1.0));
	return fuelRate * 50000.0 * efficiency;
}
}
