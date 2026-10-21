#pragma once
#include "StationSystemsDetail.h"
namespace core
{
	struct GeneratorRampIntegral { double endpointKW{}, meanKW{}; };
	inline GeneratorRampIntegral integrateGeneratorRamp(double startKW, double targetKW,
		double rampKWPerSecond, double seconds)
	{
		auto const needed = std::abs(targetKW - startKW) / rampKWPerSecond;
		auto const rampSeconds = std::min(seconds, needed);
		auto const endpoint = seconds >= needed ? targetKW
			: startKW + std::copysign(rampKWPerSecond * rampSeconds, targetKW - startKW);
		return {endpoint, (startKW * 0.5 + endpoint * 0.5) * (rampSeconds / seconds)
			+ targetKW * ((seconds - rampSeconds) / seconds)};
	}
uint64_t stationOutageOccurrencesStarted(double, double, double, double);
uint64_t stationOutageOccurrencesCompleted(double, double, double, double);
	struct PassiveExchangeLeaf
	{
		double capacityKJPerK{0.0};
		double conductanceKWPerK{0.0};
		double temperatureC{0.0};
		double weightKJPerK{0.0};
	};
	// Integrate one reservoir's whole exchange star over the tick
	// implicitly and return the reservoir's coupled end-of-tick
	// temperature, filling each leaf's weight.
	//
	// A passive conductance is solved against the temperature difference
	// *during* the tick, not the one it started with. Solving every edge
	// against the reservoir's end-of-tick temperature rather than its
	// tick-start temperature is what makes all the leaves sharing a
	// reservoir compete for the same finite heat instead of each draining
	// a temperature none of them ever sees move. Eliminating the leaves
	// from that implicit system leaves a closed form: with coupling
	// a_i = dt*G_i/C_i and weight w_i = dt*G_i/(1 + a_i), the reservoir
	// ends at the weighted average of the tick-start temperatures, and
	// each leaf's heat is w_i x (that average - its own temperature).
	//
	// Every weight is non-negative and each new temperature is a weighted
	// average of temperatures that already existed, so the exchange can
	// approach the coupled equilibrium as closely as the conductance
	// allows but can never cross it: no reversing a hot and a cold body,
	// no amplifying their separation, and nothing driven below absolute
	// zero by a stiff conductance. The heat a leaf gains is exactly the
	// negation of what the reservoir gives up, so both ledgers close on
	// the same number rather than on two independently rounded ones.
	struct TransportExchange
	{
		StationConnectionLedger::Port forward{}, backward{};
		double leafKJ{}, hubKJ{};
	};
	struct VapourThermalCoupling
	{
		double temperatureC{0.0};
		double condensedKg{0.0};
	};
	double solvePassiveExchange(std::vector<PassiveExchangeLeaf>& leaves,
		double reservoirCapacityKJPerK, double reservoirTemperatureC, double dt);
	double passiveExchangeHeatKJ(PassiveExchangeLeaf const& leaf, double coupledReservoirTemperatureC);
	void solveTransportExchange(std::vector<PassiveExchangeLeaf> const& leaves,
		std::vector<TransportExchange>& ports, double hubCapacity, double hubTemperature,
		double dt, StationConnectionLedger& connections);
	VapourThermalCoupling solveVapourThermalCoupling(double capacityKJPerK,
		double startTemperatureC, double externalHeatKW, double vapourKg,
		double volumeM3, double dt);
}
