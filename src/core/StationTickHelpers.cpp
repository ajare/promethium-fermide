#include "StationSystemsImpl.h"

namespace core
{
	void StationSystemsRuntime::Impl::Tick::initializeBuses()
	{
		for (auto const& bus : runtime.settings.powerBuses) busBudgets.try_emplace(bus.id);
		auto bind = [&](char const* type, auto const& entries) {
			for (auto const& entry : entries)
			{
				// A Single or Shared consumer draws from its declared bus. A
				// Transfer consumer sits on whichever side of its automatic
				// transfer switch the last committed tick left it on (#882);
				// transferSupply() may move it again this tick, before any draw.
				// A load with fewer than two declared buses has nothing to switch
				// to, so it stays on the first one.
				auto bus = entry.authored.supplyBusIds.front();
				if (entry.authored.supplyMode == StationSupplyMode::Transfer
					&& entry.authored.supplyBusIds.size() > 1)
				{
					auto const state = runtime.supplyTransfers.find({type, entry.authored.id});
					if (state != runtime.supplyTransfers.end() && state->second.onBackup)
						bus = entry.authored.supplyBusIds[1];
				}
				consumerBuses[{type, entry.authored.id}] = bus;
				// #883: a Shared consumer draws from every bus it was authored on,
				// simultaneously. The split across them is decided per draw from
				// each bus's live spare, so nothing is pre-allocated here; only the
				// eligible set is recorded. Anything with fewer than two declared
				// buses has nothing to combine, so it keeps the single-bus path.
				if (entry.authored.supplyMode == StationSupplyMode::Shared
					&& entry.authored.supplyBusIds.size() > 1)
					sharedSupplyBuses[{type, entry.authored.id}] = entry.authored.supplyBusIds;
				consumerSupply[{type, entry.authored.id}] = {};
			}
		};
		bind("Equipment", runtime.loads); bind("Generator", runtime.generators);
		bind("Coolant loop", runtime.coolantLoops); bind("Chiller", runtime.chillers);
		bind("Water treatment", runtime.waterTreatments); bind("Electrolyzer", runtime.electrolyzers);
		bind("CO2 capture", runtime.co2Captures); bind("Sabatier", runtime.sabatierReactors);
		bind("Methane processor", runtime.methaneProcessors); bind("Microbial processor", runtime.microbialProcessors);
		bind("Crops", runtime.cropPopulations); bind("Algae", runtime.algaeCultures);
	}

	std::string const& StationSystemsRuntime::Impl::Tick::consumerBus(std::string const& type, std::string const& id)
	{
		return consumerBuses.at({type, id});
	}

	void StationSystemsRuntime::Impl::Tick::addGeneration(std::vector<std::string> const& ids, double power)
	{
		// Multiple landings expose ONE finite source to the member buses.
		// Actual allocation decides which landing carries each watt.
		if (ids.size() > 1) { sharedGeneration.push_back({ids, power}); return; }
		auto& bus = busBudgets.at(ids.front());
		bus.generated += power;
		bus.available += power;
	}

	double StationSystemsRuntime::Impl::Tick::generationAvailable(std::string const& id)
	{
		double total = busBudgets.at(id).available;
		for (auto const& source : sharedGeneration)
			if (std::find(source.ids.begin(), source.ids.end(), id) != source.ids.end()) total += source.available;
		return total;
	}

	double StationSystemsRuntime::Impl::Tick::takeGeneration(std::string const& id, double need,
		std::vector<double> const* sharedLimits)
	{
		auto& bus = busBudgets.at(id);
		auto const local = std::min(need, bus.available);
		bus.available -= local;
		double remaining = need - local;
		double delivered = local;
		for (size_t i = 0; i < sharedGeneration.size(); ++i)
		{
			auto& source = sharedGeneration[i];
			if (std::find(source.ids.begin(), source.ids.end(), id) == source.ids.end()) continue;
			auto const limit = sharedLimits ? (*sharedLimits)[i] : source.available;
			auto const take = std::min({remaining, source.available, limit});
			source.available -= take; remaining -= take; bus.generated += take; delivered += take;
		}
		return delivered;
	}

	void StationSystemsRuntime::Impl::Tick::settleGeneration()
	{
		// Unused availability is not service. Attribute only that remaining
		// capacity equally for the surplus rollup, after all real draws.
		for (auto& source : sharedGeneration)
		{
			for (auto const& id : source.ids)
			{
				auto& bus = busBudgets.at(id);
				bus.generated += source.available / source.ids.size();
				bus.available += source.available / source.ids.size();
			}
			source.available = 0;
		}
	}

	double StationSystemsRuntime::Impl::Tick::sourceDemand(std::vector<std::string> const& ids)
	{
		if (runtime.settings.powerBuses.size() == 1) return busBudgets.at(ids.front()).dispatch;
		double result = 0;
		for (auto const& id : ids) result += std::max(0.0, busBudgets.at(id).dispatch);
		return result;
	}

	void StationSystemsRuntime::Impl::Tick::commitSource(std::vector<std::string> const& ids, double power)
	{
		if (runtime.settings.powerBuses.size() == 1) { busBudgets.at(ids.front()).dispatch -= power; return; }
		for (auto const& id : ids)
		{
			auto& bus = busBudgets.at(id);
			auto const committed = std::min(power, bus.dispatch);
			bus.dispatch -= committed; power -= committed;
		}
	}

	ConstituentLedger StationSystemsRuntime::Impl::Tick::drawStoreActual(size_t storeIndex, double totalKgBefore, double drawnKg)
	{
		auto const& store = runtime.stores[storeIndex].authored;
		auto actual = ConstituentLedger{};
		if (store.composition)
			addComposition(actual, *store.composition, drawnKg, 1.0);
		auto& drift = stagedDrift[storeIndex];
		if (!drift.empty() && totalKgBefore > 0.0 && drawnKg > 0.0)
		{
			auto const share = std::min(1.0, drawnKg / totalKgBefore);
			for (auto& entry : drift)
			{
				actual[entry.first] += share * entry.second;
				entry.second *= 1.0 - share;
			}
		}
		return actual;
	}

	StationWaterQualitySnapshot StationSystemsRuntime::Impl::Tick::waterQuality(size_t index,
		WaterQualitySettings const& gate, double demandKg, char const* consumer, bool pureFeed)
	{
		StationWaterQualitySnapshot result;
		result.enabled = pureFeed || !gate.maxConcentrations.empty() || gate.maxTotalDissolved.has_value();
		result.demandKgPerS = demandKg / static_cast<double>(seconds);
		auto const& store = runtime.stores[index];
		if (store.authored.composition)
		{
			// Read tracked species directly. Reconstructing dry organics as
			// 1 - moisture can invent an ulp of contamination in pure water
			// and incorrectly close a zero-tolerance acceptance gate.
			auto const totalKg = massToKg(store.authored.unit, qualified[index] + unqualified[index]);
			auto const basisKg = totalKg > 0.0 ? totalKg : 1.0;
			auto blend = compositionLedgerOf(*store.authored.composition, basisKg, 1.0);
			if (totalKg > 0.0) addLedger(blend, stagedDrift[index], 1.0);
			for (auto const* name : StationWaterDryConstituents)
			{
				auto const found = blend.find(name);
				auto const value = found == blend.end() ? 0.0 : std::max(0.0, found->second / basisKg);
				result.concentrations[name] = value;
				result.totalDissolved += value;
			}
		}
		auto const limit = [&](double concentration, double tolerance, std::string const& name)
		{
			if (concentration <= tolerance) return;
			auto const fraction = tolerance / concentration;
			if (fraction < result.scale)
			{
				result.scale = fraction;
				result.bindingConcentration = concentration;
				result.bindingTolerance = tolerance;
				result.limiter = "water quality: " + name + " above " + consumer + " tolerance";
			}
		};
		for (auto const& [name, tolerance] : gate.maxConcentrations)
			limit(result.concentrations[name], tolerance, name);
		if (gate.maxTotalDissolved || pureFeed)
			limit(result.totalDissolved, pureFeed ? 0.0 : *gate.maxTotalDissolved, "total dissolved");
		result.permittedKgPerS = result.demandKgPerS * result.scale;
		return result;
	}

	ResourceCompositionSettings StationSystemsRuntime::Impl::Tick::storeComposition(size_t index)
	{
		return effectiveCompositionOf(*runtime.stores[index].authored.composition,
			massToKg(runtime.stores[index].authored.unit, qualified[index] + unqualified[index]),
			stagedDrift[index]);
	}

	void StationSystemsRuntime::Impl::Tick::depositStoreDrift(size_t storeIndex, double depositedKg, ConstituentLedger const& actual)
	{
		auto const& store = runtime.stores[storeIndex].authored;
		if (!store.composition) return;
		auto& drift = stagedDrift[storeIndex];
		addLedger(drift, actual, 1.0);
		addLedger(drift, compositionLedgerOf(*store.composition, depositedKg, 1.0), -1.0);
	}

	void StationSystemsRuntime::Impl::Tick::moveStoreDrift(size_t sourceIndex, size_t destinationIndex, double sourceKgBefore, double movedKg)
	{
		if (!runtime.stores[sourceIndex].authored.composition
			&& runtime.stores[destinationIndex].authored.composition)
		{
			depositStoreDrift(destinationIndex, movedKg, {{"water", movedKg}});
			stagedLoss["water"] -= movedKg;
			return;
		}
		auto& source = stagedDrift[sourceIndex];
		if (source.empty() || sourceKgBefore <= 0.0 || movedKg <= 0.0) return;
		auto const share = std::min(1.0, movedKg / sourceKgBefore);
		auto& destination = stagedDrift[destinationIndex];
		for (auto& entry : source)
		{
			destination[entry.first] += share * entry.second;
			entry.second *= 1.0 - share;
		}
	}

	StationMaterialConnections StationSystemsRuntime::Impl::Tick::materialPorts(std::string const& type, std::string const& id)
	{
		auto const actualOutput = type == "Crops" || type == "Algae" || type == "Water treatment";
		return StationMaterialConnections(connections, runtime.settings, type, id, seconds,
			flowIn, flowOut, qualified, materialConnectionsValid, stagedDrift,
			[&](size_t index, double lost)
			{
				if (lost <= 0.0) return;
				auto const& store = runtime.stores[index].authored;
				addLedger(stagedLoss, drawStoreActual(index, massToKg(store.unit,
					qualified[index] + unqualified[index]), massToKg(store.unit, lost)), 1.0);
				qualified[index] -= lost; flowOut[index] += lost;
				stagedUnitLoss[store.unit] += lost;
			},
			[&, actualOutput, type, id](size_t index, double produced, double lost, ConstituentLedger const& before)
			{
				if (lost <= 0.0) return;
				auto const& store = runtime.stores[index].authored;
				bool isQualified = true;
				for (auto const& treatment : runtime.waterTreatments)
				{
					if (type != "Water treatment" || treatment.authored.id != id) continue;
					if (treatment.concentrateIndex == index) isQualified = store.composition.has_value();
					if (treatment.treatedIndex == index) isQualified = treatment.lastQualityAccepted;
				}
				(isQualified ? qualified[index] : unqualified[index]) -= lost;
				flowIn[index] -= lost;
				stagedUnitLoss[store.unit] += lost;
				if (store.composition) addComposition(stagedLoss, *store.composition, massToKg(store.unit, lost), 1.0);
				if (actualOutput && produced > 0.0)
				{
					auto delta = stagedDrift[index]; addLedger(delta, before, -1.0);
					addLedger(stagedLoss, delta, lost / produced);
					addLedger(stagedDrift[index], delta, -lost / produced);
				}
			});
	}

	double StationSystemsRuntime::Impl::Tick::batteryReserve(BatteryState const& battery)
	{
		// #843: the reserve is part of the rating, so permanent Wear
		// scales it with everything else: the protected floor and the
		// cyclable slice fade together and keep their authored ratio.
		return (battery.authored.nameplateKWh - battery.authored.usableKWh)
			* battery.wearCapability();
	}

	bool StationSystemsRuntime::Impl::Tick::batteryDisabled(BatteryState const& battery)
	{
		// #845: a bank in thermal runaway is held out of service for the
		// whole Fault: it takes no charge and delivers no discharge, the
		// same hard gate a zero-usable bank gets, so no allocation path
		// can route power through a bank that is coming apart. Its stored
		// energy stays fully accounted and stranded while the Fault
		// stands; the hazard it booked landed in the compartment, not
		// here.
		return battery.effectiveUsableKWh() <= 0.0 || battery.thermalRunaway.has_value();
	}

	double StationSystemsRuntime::Impl::Tick::nativeDeliverableKW(BatteryState const& battery, double dtHours)
	{
		if (batteryDisabled(battery)) return 0.0;
		// #843: a faded bank delivers from no more than its faded ceiling:
		// energy stored above the effective nameplate is stranded by the
		// fade, still fully accounted in `stored`, never deliverable.
		auto const drawable = std::max(0.0,
			std::min(battery.stored, battery.effectiveNameplateKWh()) - batteryReserve(battery));
		auto const byEnergy = drawable * battery.authored.dischargeEfficiency / dtHours;
		// The discharge power limit is per tick: whatever this battery
		// already delivered this tick counts against it, so the two
		// allocation passes can never jointly exceed the cap.
		auto const byPower = battery.authored.maxDischargeKW - battery.lastDischargeKW;
		return std::max(0.0, std::min(byPower, byEnergy));
	}

	double StationSystemsRuntime::Impl::Tick::deliverableKW(BatteryState const& battery, double dtHours)
	{
		return connections.available(connections.port("Battery", battery.authored.id, "Discharge"),
			nativeDeliverableKW(battery, dtHours));
	}

	double StationSystemsRuntime::Impl::Tick::batteryDeliverableKW(std::string const& busId)
	{
		double total{0.0};
		for (auto const& battery : runtime.batteries)
			if (std::find(battery.authored.busIds.begin(), battery.authored.busIds.end(), busId) != battery.authored.busIds.end())
				total += deliverableKW(battery, hours);
		return total;
	}

	double StationSystemsRuntime::Impl::Tick::drawBatteries(std::string const& busId, double needKW,
		std::vector<std::pair<size_t, double>>* contributions, std::vector<double> const* limits)
	{
		if (needKW <= 0.0) return 0.0;
		auto remaining = needKW;
		for (size_t batteryIndex = 0; batteryIndex < runtime.batteries.size(); ++batteryIndex)
		{
			auto& battery = runtime.batteries[batteryIndex];
			if (remaining <= 1e-15) break;
			if (batteryDisabled(battery)) continue;
			if (std::find(battery.authored.busIds.begin(), battery.authored.busIds.end(), busId) == battery.authored.busIds.end()) continue;
			auto const port = connections.port("Battery", battery.authored.id, "Discharge");
			auto const claim = limits ? std::min(remaining, (*limits)[batteryIndex]) : remaining;
			auto const request = connections.required(port, claim);
			auto const transport = connections.receive(port, claim, nativeDeliverableKW(battery, hours));
			auto const give = transport.delivered;
			if (transport.input > 0.0)
			{
				auto const drawn = transport.input * hours
					/ battery.authored.dischargeEfficiency;
				battery.stored -= drawn;
				battery.dischargedKWh += drawn;
				battery.deliveredKWh += transport.input * hours;
				battery.lastDischargeKW += transport.input;
				// #843: cycles are counted on real discharge. A full
				// charge plus a full discharge of the authored cyclable
				// energy is one equivalent cycle, so each side of the
				// round trip banks half of what it moves.
				if (battery.authored.capacityFade && battery.authored.usableKWh > 0.0)
					advanceWear(battery.wear, *battery.authored.capacityFade,
						drawn / (2.0 * battery.authored.usableKWh), 0.0, 0.0, 0.0);
				if (contributions != nullptr)
					contributions->push_back({batteryIndex, give});
			}
			battery.lastDischargeRequestedKW += request;
			battery.lastDischargeShortfallKW += request - transport.input;
			remaining -= give;
		}
		return needKW - remaining;
	}

	StationConnectionLedger::Port StationSystemsRuntime::Impl::Tick::powerPort(std::string const& type, std::string const& id)
	{
		// Bus-agnostic: the consumer's supply role however it is wired. Callers
		// that care which bus a watt travels to use the bus-scoped overload,
		// which is the only way to pick the right wire when a consumer owns
		// several. This form deliberately resolves no bus, so it stays usable
		// from the ledger's preview passes, where the tick's bus bindings for a
		// given consumer may not have been taken yet.
		return connections.port(type, id, type == "Generator" ? "Startup" : "Electrical supply");
	}

	StationConnectionLedger::Port StationSystemsRuntime::Impl::Tick::powerPort(std::string const& type,
		std::string const& id, std::string const& busId)
	{
		// The consumer's own supply wire to this one bus. A Shared load owns one
		// of these per bus and each is its own transport, so a pro-rata draw
		// books its throughput and its loss against the cable it really ran down.
		return connections.port(type, id,
			type == "Generator" ? "Startup" : "Electrical supply", busId);
	}

	std::vector<std::string> StationSystemsRuntime::Impl::Tick::supplyBuses(
		std::string const& type, std::string const& id) const
	{
		auto const shared = sharedSupplyBuses.find(std::pair<std::string, std::string>{type, id});
		if (shared != sharedSupplyBuses.end()) return shared->second;
		return { consumerBuses.at({type, id}) };
	}

	bool StationSystemsRuntime::Impl::Tick::supplyDisconnected(
		std::string const& type, std::string const& id)
	{
		// Dead only when every wire is dead. A Shared load with one live feed and
		// one severed one still runs on the live feed, so it must not be pushed
		// down the "no connection at all" path.
		for (auto const& busId : supplyBuses(type, id))
			if (connections.efficiency(powerPort(type, id, busId)) != 0.0) return false;
		return true;
	}

	double StationSystemsRuntime::Impl::Tick::powerAvailable(std::string const& type, std::string const& id)
	{
		// #883: a Shared load sees the combined capacity of every bus it is
		// wired to. That combined figure is also the bar the minimum-operable
		// rule is measured against, so the rule binds the grant as a whole:
		// a Shared load is never partly energized below its floor.
		double total = 0.0;
		for (auto const& capacity : supplyCapacity(type, id))
		{
			total += capacity.spareKW;
			if (sharedSupplyBuses.contains({type, id}))
				supplyFeed(type, id, capacity.busId).spareKW = capacity.spareKW;
		}
		return total;
	}

	std::vector<StationSystemsRuntime::Impl::Tick::SupplyCapacity>
	StationSystemsRuntime::Impl::Tick::supplyCapacity(std::string const& type,
		std::string const& id, bool includeBatteries)
	{
		auto const buses = supplyBuses(type, id);
		std::vector<SupplyCapacity> result;
		std::vector<double> input, room;
		for (auto const& busId : buses)
		{
			auto const port = powerPort(type, id, busId);
			auto const cap = connections.efficiency(port) > 0.0 ? connections.remaining(port) : 0.0;
			auto const local = std::min(cap, busBudgets.at(busId).available);
			result.push_back({busId, 0.0, std::vector<double>(sharedGeneration.size()),
				std::vector<double>(runtime.batteries.size())});
			input.push_back(local);
			room.push_back(cap - local);
		}
		// Each multi-landing resource offers ONE budget. Partition it over
		// eligible wires, redistributing a capped/dead wire's slice. Keep these
		// per-resource ceilings for the real draw so an early bus cannot spend
		// a later bus's offer. Preview and execution then agree at the operable
		// minimum even when generation or a battery bridges several buses.
		auto offer = [&](std::vector<std::string> const& members, double available,
			size_t resource, bool battery) {
			std::vector<size_t> eligible;
			for (size_t i = 0; i < buses.size(); ++i)
				if (room[i] > 0.0 && std::find(members.begin(), members.end(), buses[i]) != members.end())
					eligible.push_back(i);
			while (available > 0.0 && !eligible.empty())
			{
				auto const slice = available / eligible.size();
				bool capped = false;
				for (auto i : eligible)
				{
					auto const give = std::min(slice, room[i]);
					(battery ? result[i].batteryLimits : result[i].generationLimits)[resource] += give;
					input[i] += give;
					room[i] -= give;
					available -= give;
					capped |= room[i] <= 0.0;
				}
				if (!capped) break;
				std::erase_if(eligible, [&](size_t i) { return room[i] <= 0.0; });
			}
		};
		for (size_t i = 0; i < sharedGeneration.size(); ++i)
			offer(sharedGeneration[i].ids, sharedGeneration[i].available, i, false);
		if (includeBatteries)
			for (size_t i = 0; i < runtime.batteries.size(); ++i)
				offer(runtime.batteries[i].authored.busIds, deliverableKW(runtime.batteries[i], hours), i, true);
		for (size_t i = 0; i < result.size(); ++i)
			result[i].spareKW = input[i] * connections.efficiency(powerPort(type, id, buses[i]));
		return result;
	}

	typename StationSystemsRuntime::Impl::Tick::BusSupply&
	StationSystemsRuntime::Impl::Tick::supplyFeed(std::string const& type, std::string const& id,
		std::string const& busId)
	{
		auto& feeds = consumerSupply[{type, id}];
		auto found = std::find_if(feeds.begin(), feeds.end(),
			[&](auto const& feed) { return feed.busId == busId; });
		if (found == feeds.end()) found = feeds.insert(feeds.end(), BusSupply{busId, {}, {}, {}, {}, {}});
		return *found;
	}

	double StationSystemsRuntime::Impl::Tick::drawPowerFromBus(std::string const& type, std::string const& id,
		std::string const& busId, double needKW, std::vector<std::pair<size_t, double>>* contributions,
		double* generation, SupplyCapacity const* capacity)
	{
		auto const port = powerPort(type, id, busId);
		auto const grossNeed = connections.required(port, needKW);
		auto& bus = busBudgets.at(busId);
		auto const fromGeneration = takeGeneration(busId, grossNeed,
			capacity ? &capacity->generationLimits : nullptr);
		availableGeneration -= fromGeneration;
		std::vector<std::pair<size_t, double>> busBattery;
		auto const fromBattery = drawBatteries(busId, grossNeed - fromGeneration, &busBattery,
			capacity ? &capacity->batteryLimits : nullptr);
		if (contributions != nullptr)
			for (auto const& entry : busBattery) contributions->push_back(entry);
		bus.discharged += fromBattery;
		bus.input += fromGeneration + fromBattery;
		batterySuppliedKW += fromBattery;
		batteryShortfallKW += grossNeed - fromGeneration - fromBattery;
		if (generation) *generation += fromGeneration;
		auto const delivered = connections.send(port, fromGeneration + fromBattery).delivered;
		consumerSupplied[{type, id}] += delivered;
		// Record the slice against the bus that carried it. The audit, the
		// published split and any refund of it all read back through here, so
		// "what this bus did" is never re-derived from what was asked.
		auto& feed = supplyFeed(type, id, busId);
		feed.requestedKW += needKW;
		feed.deliveredKW += delivered;
		feed.fromGenerationKW += fromGeneration;
		for (auto const& entry : busBattery)
		{
			auto hit = std::find_if(feed.fromBatteryKW.begin(), feed.fromBatteryKW.end(),
				[&](auto const& claim) { return claim.first == entry.first; });
			if (hit == feed.fromBatteryKW.end()) feed.fromBatteryKW.push_back(entry);
			else hit->second += entry.second;
		}
		return delivered;
	}

	double StationSystemsRuntime::Impl::Tick::drawPower(std::string const& type, std::string const& id, double needKW, std::vector<std::pair<size_t, double>>* contributions, double* generation)
	{
		auto const buses = supplyBuses(type, id);
		if (generation) *generation = 0.0;
		if (buses.size() <= 1)
			return drawPowerFromBus(type, id, buses.front(), needKW, contributions, generation);
		// #883: the Shared split. Each bus is asked for the slice of this demand
		// its own spare can carry, weighed right now - after the protected
		// services ahead of it have taken theirs and after every load allocated
		// earlier in this same pass has taken its. Nothing is carried over from
		// last tick: generation moves, so the split moves with it.
		auto const capacities = supplyCapacity(type, id);
		std::vector<double> spare;
		double totalSpare = 0.0;
		for (auto const& capacity : capacities)
		{
			spare.push_back(capacity.spareKW);
			totalSpare += capacity.spareKW;
		}
		std::vector<double> shares(buses.size(), 0.0);
		if (totalSpare > 0.0)
		{
			// Pro-rata by spare, capped at that bus's spare. When the demand fits,
			// every bus gives its proportional slice. When it does not, the cap
			// binds everywhere at once, each bus empties into the load, and what
			// is still missing is the load's own explicit unmet - never a silent
			// clamp hidden on one circuit.
			for (size_t i = 0; i < buses.size(); ++i)
				shares[i] = std::min(spare[i], needKW * spare[i] / totalSpare);
		}
		double delivered = 0.0;
		for (size_t i = 0; i < buses.size(); ++i)
		{
			// Publish what each bus was weighed at even when it carried nothing,
			// so a reader can see the zero-spare bus take its zero share rather
			// than simply being absent from the split.
			supplyFeed(type, id, buses[i]).spareKW = spare[i];
			// A bus with no spare takes no share: the others already absorbed it
			// by carrying proportionally more of a fixed demand.
			if (shares[i] <= 0.0) continue;
			delivered += drawPowerFromBus(type, id, buses[i], shares[i], contributions, generation, &capacities[i]);
		}
		return delivered;
	}

	std::vector<std::pair<std::string, double>> StationSystemsRuntime::Impl::Tick::supplyAttribution(
		std::string const& type, std::string const& id) const
	{
		auto const feeds = consumerSupply.find(std::pair<std::string, std::string>{type, id});
		double total = 0.0;
		if (feeds != consumerSupply.end())
			for (auto const& feed : feeds->second) total += feed.requestedKW;
		if (feeds != consumerSupply.end() && total > 0.0)
		{
			std::vector<std::pair<std::string, double>> result;
			result.reserve(feeds->second.size());
			for (auto const& feed : feeds->second)
				result.push_back({feed.busId, feed.requestedKW / total});
			return result;
		}
		// Nothing was ever asked of any bus this tick. Spread the consumer's
		// demand equally across the buses it is wired to, so a load refused
		// whole still shows up on every circuit it asked, and a single-bus
		// consumer stays wholly on its one bus.
		std::vector<std::pair<std::string, double>> result;
		auto const buses = supplyBuses(type, id);
		if (buses.empty()) return result;
		for (auto const& busId : buses)
			result.push_back({busId, 1.0 / static_cast<double>(buses.size())});
		return result;
	}

	double StationSystemsRuntime::Impl::Tick::energyUnitsToKWh(std::string const& unit)
	{
		return stationEnergyUnitsToKWh(unit);
	}

	double StationSystemsRuntime::Impl::Tick::startupDrawForKW(GeneratorState const& generator, double requiredKJ)
	{
		if (requiredKJ <= 0.0) return 0.0;
		auto const remainingKJ = std::max(0.0,
			requiredKJ - generator.startupReceivedKJ);
		return dispatchDtSeconds > 0.0
			? std::min(generator.authored.startupPowerKW,
				remainingKJ / dispatchDtSeconds)
			: generator.authored.startupPowerKW;
	}

	double StationSystemsRuntime::Impl::Tick::exchangeFullAmount(LoadExchangeState const& exchange, double profileLevel)
	{
		return exchange.ratePerSecond * static_cast<double>(seconds) * profileLevel;
	}

	StationConnectionLedger::Port StationSystemsRuntime::Impl::Tick::exchangePort(LoadState const& load, LoadExchangeState const& exchange)
	{
		size_t ordinal = 0;
		for (auto const& item : load.exchanges)
		{
			if (item.consumes == exchange.consumes) ++ordinal;
			if (&item == &exchange) break;
		}
		return connections.port("Equipment", load.authored.id,
			std::string(exchange.consumes ? "Material in " : "Material return ") + std::to_string(ordinal));
	}

	double StationSystemsRuntime::Impl::Tick::materialFeasible(LoadState const& load, std::string& limiter, bool requesting)
	{
		double fraction = 1.0;
		auto const feedLimiter = [&](size_t index, double drawable) {
			auto const& store = runtime.stores[index].authored;
			return drawable <= 1e-12
				? (store.protectedReserve > 0.0
					? "feed store '" + store.id + "' is at its protected reserve"
					: (unqualified[index] > 1e-12
						? "feed store '" + store.id + "' holds only unqualified material"
						: "feed store '" + store.id + "' is empty"))
				: "feed store '" + store.id + "' supply limits the operating level";
		};
		std::map<size_t, double> feedClaims, returnClaims;
		for (auto const& exchange : load.exchanges)
		{
			auto const& store = runtime.stores[exchange.storeIndex].authored;
			auto const full = exchangeFullAmount(exchange, load.lastProfileLevel);
			if (full <= 0.0) continue;
			auto const port = exchangePort(load, exchange);
			auto const eta = connections.efficiency(port);
			if (exchange.consumes)
			{
				auto const drawable = std::max(0.0, qualified[exchange.storeIndex] - store.protectedReserve);
				auto const available = connections.available(port, drawable / seconds) * seconds;
				auto const requestAvailable = requesting && eta == 0.0
					? std::min(drawable, connections.remaining(port) * seconds) : available;
				if (requestAvailable / full < fraction)
				{
					fraction = requestAvailable / full;
					// A per-port claim may bind before the shared-store pass
					// sees the same ratio; preserve the inventory diagnosis.
					limiter = drawable <= 1e-12 || (eta > 0.0 && drawable <= connections.remaining(port) * seconds)
						? feedLimiter(exchange.storeIndex, drawable)
						: "feed store '" + store.id + "' physical connection limits the operating level";
				}
				if (eta > 0.0 || requesting) feedClaims[exchange.storeIndex] += full / (eta > 0.0 ? eta : 1.0);
			}
			else
			{
				auto const capacity = connections.remaining(port) * seconds;
				if (capacity / full < fraction)
				{
					fraction = capacity / full;
					limiter = "destination physical connection throughput limits the operating level";
				}
				returnClaims[exchange.storeIndex] += full * eta;
			}
		}
		for (auto const& [index, full] : feedClaims)
		{
			auto const& store = runtime.stores[index].authored;
			auto const drawable = std::max(0.0, qualified[index] - store.protectedReserve);
			auto const ratio = drawable / full;
			if (ratio < fraction) { fraction = ratio; limiter = feedLimiter(index, drawable); }
		}
		for (auto const& [index, full] : returnClaims)
		{
			auto const& store = runtime.stores[index].authored;
			auto const room = std::max(0.0, store.capacity - qualified[index] - unqualified[index]);
			auto const ratio = room <= 0.0 ? 0.0 : full > 0.0 ? room / full : 1.0;
			if (ratio < fraction)
			{
				fraction = ratio;
				limiter = room <= 1e-12 ? "destination store '" + store.id + "' is full"
					: "destination store '" + store.id + "' headroom limits the operating level";
			}
		}
		return std::clamp(fraction, 0.0, 1.0);
	}

	double StationSystemsRuntime::Impl::Tick::busSupplied(std::string const& type, std::string const& id,
		std::string const& busId) const
	{
		auto const feeds = consumerSupply.find(std::pair<std::string, std::string>{type, id});
		if (feeds == consumerSupply.end()) return 0.0;
		auto const hit = std::find_if(feeds->second.begin(), feeds->second.end(),
			[&](auto const& feed) { return feed.busId == busId; });
		return hit == feeds->second.end() ? 0.0 : hit->deliveredKW;
	}

	double StationSystemsRuntime::Impl::Tick::addStartupDispatch(std::string const& type,
		std::string const& id, double crankKW)
	{
		double total = 0.0;
		for (auto const& [busId, weight] : supplyAttribution(type, id))
		{
			auto const busCrank = connections.inputDemand(
				powerPort(type, id, busId), crankKW * weight);
			busBudgets.at(busId).startup += busCrank;
			total += busCrank;
		}
		return total;
	}

	void StationSystemsRuntime::Impl::Tick::reserveMaterial(LoadState& load, double fraction)
	{
		for (auto& exchange : load.exchanges)
		{
			auto const port = exchangePort(load, exchange);
			auto const device = exchangeFullAmount(exchange, load.lastProfileLevel) * fraction;
			auto const reserved = exchange.consumes ? connections.required(port, device / seconds) * seconds
				: device * connections.efficiency(port);
			qualified[exchange.storeIndex] += (exchange.consumes ? -1.0 : 1.0) * reserved;
			exchange.lastReserved = reserved;
		}
		load.lastMaterialReserved = true;
	}

	void StationSystemsRuntime::Impl::Tick::leakZeroMaterial(LoadState const& load)
	{
		if (std::none_of(load.exchanges.begin(), load.exchanges.end(), [&](auto const& exchange) {
			return exchange.consumes && connections.efficiency(exchangePort(load, exchange)) == 0.0;
		})) return;
		std::string ignored;
		auto const material = materialFeasible(load, ignored, true);
		auto const minimum = load.lastRequestedKW * (1.0 - load.authored.maxCurtailment);
		auto const power = powerAvailable("Equipment", load.authored.id);
		if (load.lastRequestedKW <= 0.0 || power <= 0.0 || material <= 0.0
			|| material * load.lastRequestedKW + 1e-12 < minimum || power + 1e-12 < minimum) return;
		auto const demandScale = std::min(material, power / load.lastRequestedKW);
		for (auto const& exchange : load.exchanges)
		{
			auto const port = exchangePort(load, exchange);
			if (!exchange.consumes || connections.efficiency(port) > 0.0) continue;
			auto const index = exchange.storeIndex;
			auto const& store = runtime.stores[index].authored;
			auto const drawable = std::max(0.0, qualified[index] - store.protectedReserve);
			auto const lost = std::min(drawable, connections.receive(port,
				exchange.ratePerSecond * load.lastProfileLevel * demandScale, drawable / seconds).input * seconds);
			double pending = 0.0;
			for (auto const& other : runtime.loads)
				for (auto const& item : other.exchanges)
					if (item.storeIndex == index) pending += (item.consumes ? 1.0 : -1.0) * item.lastReserved;
			addLedger(stagedLoss, drawStoreActual(index, massToKg(store.unit,
				qualified[index] + unqualified[index] + pending), massToKg(store.unit, lost)), 1.0);
			qualified[index] -= lost; flowOut[index] += lost; stagedUnitLoss[store.unit] += lost;
		}
	}

	// #749: the single named home of the protected-service decision. A
	// coolant loop's circulation and freeze protection is a protected
	// Critical station service when - and only when - the author declares
	// the loop a critical service (#754). The loop carries no class
	// (#757); protection comes only from `criticalService`.
	// Every protection and thermal pass calls this instead
	// of re-deriving the rule inline, so the decision lives in one
	// reviewable place.
	bool StationSystemsRuntime::Impl::Tick::coolantServiceProtected(LoopState const& loop)
	{
		return loop.authored.criticalService;
	}

	void StationSystemsRuntime::Impl::Tick::recordProtected(std::string type, std::string id, std::string kind, double requestedKW, double suppliedKW, std::string reason)
	{
		if (requestedKW <= 0.0) return;
		// #883: a protected service on a Shared consumer is carried by several
		// buses at once, so it is attributed to each of them by the share it
		// actually took rather than pinned to whichever bus is listed first.
		protectedServiceBuses.push_back(supplyAttribution(type, id));
		protectedServices.push_back({std::move(id), std::move(kind), requestedKW,
			suppliedKW, std::max(0.0, requestedKW - suppliedKW), std::move(reason)});
		protectedDemandKW += requestedKW;
		protectedSuppliedKW += suppliedKW;
	}

	void StationSystemsRuntime::Impl::Tick::refundChillerCredit(size_t chillerIndex, double refundKW)
	{
		if (refundKW <= 1e-15) return;
		auto const& id = runtime.chillers[chillerIndex].authored.id;
		auto const key = std::pair<std::string, std::string>{"Chiller", id};
		auto const draws = consumerSupply.find(key);
		if (draws == consumerSupply.end()) return;
		double totalCredit = 0.0;
		for (auto const& feed : draws->second) totalCredit += feed.deliveredKW;
		if (totalCredit <= 0.0) return;
		auto const refundFraction = std::min(1.0, refundKW / totalCredit);
		// Every bus gives back the same fraction of what it carried. A Shared chiller's
		// refund therefore returns to the circuits that funded it, so one bus
		// never absorbs another's unused work and each circuit's own ledger
		// still closes. The credit is always the chiller's first draw this tick,
		// so what these feeds hold is exactly the credit and nothing else.
		for (auto feed = draws->second.rbegin();
			feed != draws->second.rend() && refundKW > 1e-15; ++feed)
		{
			auto const port = powerPort("Chiller", id, feed->busId);
			auto const eta = connections.efficiency(port);
			if (eta <= 0.0) continue;
			auto const giveDelivered = std::min(refundKW, feed->deliveredKW * refundFraction);
			if (giveDelivered <= 1e-15) continue;
			auto const giveInput = giveDelivered / eta;
			connections.refund(port, giveInput);
			auto& bus = busBudgets.at(feed->busId);
			bus.input -= giveInput;
			consumerSupplied[key] -= giveDelivered;
			feed->deliveredKW -= giveDelivered;
			auto remaining = giveInput;
			for (auto it = feed->fromBatteryKW.rbegin();
				it != feed->fromBatteryKW.rend() && remaining > 1e-15; ++it)
			{
				auto const give = std::min(remaining, it->second);
				if (give <= 0.0) continue;
				auto& battery = runtime.batteries[it->first];
				auto const dischargePort = connections.port("Battery", battery.authored.id, "Discharge");
				auto const nativeGive = give / connections.efficiency(dischargePort);
				connections.refund(dischargePort, nativeGive);
				auto const storedKWh = nativeGive * hours / battery.authored.dischargeEfficiency;
				battery.stored += storedKWh;
				battery.dischargedKWh -= storedKWh;
				battery.deliveredKWh -= nativeGive * hours;
				battery.lastDischargeKW -= nativeGive;
				// #843: the refunded discharge never actually happened, so the
				// cycle it was counted toward is unwound with it. This is the
				// only unwinding Wear ever sees, and it removes operation that
				// truly did not occur - it never heals fade from work that did.
				if (battery.authored.capacityFade && battery.authored.usableKWh > 0.0)
					advanceWear(battery.wear, *battery.authored.capacityFade,
						-storedKWh / (2.0 * battery.authored.usableKWh), 0.0, 0.0, 0.0,
						previousBatteryWearFraction[it->first]);
				batterySuppliedKW -= give;
				bus.discharged -= give;
				it->second -= give;
				remaining -= give;
			}
			availableGeneration += remaining;
			bus.available += remaining;
			refundKW -= giveDelivered;
		}
	}

	void StationSystemsRuntime::Impl::Tick::reconcileChillerProtected(size_t chillerIndex)
	{
		if (chillerProtectedServiceIndex[chillerIndex] == static_cast<size_t>(-1)) return;
		auto const creditKW = protectedChillerPowerKW[chillerIndex];
		auto const actualKW = runtime.chillers[chillerIndex].lastPowerSuppliedKW;
		if (actualKW == creditKW) return;
		protectedSuppliedKW += actualKW - creditKW;
		auto& service = protectedServices[chillerProtectedServiceIndex[chillerIndex]];
		service.suppliedKW = actualKW;
		service.unmetKW = std::max(0.0, service.requestedKW - actualKW);
		if (actualKW < creditKW && service.reason.empty())
			service.reason = "the protected cooling credit was reconciled to the duty "
				"the cold loop could actually deliver: unused work was returned to the pool";
	}

	bool StationSystemsRuntime::Impl::Tick::sinkQualified(size_t index, double maximum, double minimum)
	{
		return runtime.prediction.qualifies(ProtectionPrediction::Body::Reservoir, index,
			runtime.reservoirs[index].temperatureC, minimum, maximum);
	}

	bool StationSystemsRuntime::Impl::Tick::atmosphereQualified(size_t index, double minimum, double maximum)
	{
		return runtime.prediction.qualifies(ProtectionPrediction::Body::Atmosphere, index,
			runtime.atmospheres[index].temperatureC, minimum, maximum);
	}

	void StationSystemsRuntime::Impl::Tick::captureProtection()
	{
		if (!runtime.prediction.active) return;
		for (size_t i = 0; i < runtime.electrolyzers.size(); ++i)
		{
			auto const& cell = runtime.electrolyzers[i];
			if (!cell.authored.thermalBody) continue;
			runtime.prediction.include(ProtectionPrediction::Body::Machine, i,
				protectionMachineBodyStart[i], cell.bodyTemperatureC,
				cell.authored.thermalBody->heatCapacityKJPerK, seconds,
				{cell.lastBodyChargeKW, -cell.lastSinkLegKW, cell.lastBodyConductedKW});
		}
		for (size_t i = 0; i < runtime.reservoirs.size(); ++i)
		{
			auto const& r = runtime.reservoirs[i];
			std::vector<double> rates{r.authored.absorbedHeatKW, r.lastHeatRoutedKW,
				r.lastCondenserHeatKW, r.lastGeneratorHeatKW, r.lastElectrolysisHeatKW,
				r.lastFuelCellHeatKW, r.lastSabatierHeatKW, r.lastMethaneProcessingHeatKW,
				r.lastMicrobialHeatKW, r.lastBufferDischargeKW, -r.lastBufferChargeKW,
				// Heat engine terms (#804): extraction cools the hot source and
				// rejection warms the sink, both real rate terms the next tick's
				// qualification prediction must see.
				-r.lastEngineExtractionKW, r.lastEngineRejectionKW};
			for (auto const& radiator : runtime.radiators)
				if (radiator.reservoirIndex == i) rates.push_back(-radiator.lastRejectedKW);
			// A star's aggregate exchange can hide opposing leaves. Bound both
			// signs separately; the hub net also accounts for transport loss
			// (hub cooling can exceed the heat delivered to its leaves).
			double exchangeMagnitudeKW = 0;
			for (auto const& loop : runtime.coolantLoops)
				if (loop.reservoirIndex == i) exchangeMagnitudeKW += std::abs(loop.lastExchangeIntoLoopKW);
			for (auto const& atmosphere : runtime.atmospheres)
				if (atmosphere.reservoirIndex == i) exchangeMagnitudeKW += std::abs(atmosphere.lastExchangeIntoCompartmentKW);
			rates.push_back(exchangeMagnitudeKW);
			rates.push_back(-exchangeMagnitudeKW);
			rates.push_back(r.lastLoopHeatKW + r.lastAtmosphereHeatKW);
			runtime.prediction.include(ProtectionPrediction::Body::Reservoir, i,
				protectionReservoirStart[i], r.temperatureC, r.authored.heatCapacityKJPerK, seconds, rates);
		}
		for (size_t i = 0; i < runtime.coolantLoops.size(); ++i)
		{
			auto const& loop = runtime.coolantLoops[i];
			runtime.prediction.include(ProtectionPrediction::Body::Loop, i,
				protectionLoopStart[i], loop.temperatureC,
				loop.coolantKg * loop.authored.specificHeatKJPerKgK, seconds,
				{loop.lastPumpSuppliedKW, loop.lastHeaterSuppliedKW, loop.lastExchangeIntoLoopKW,
				 loop.lastCollectorHeatKW, -loop.lastChillerExtractionKW});
		}
		for (size_t i = 0; i < runtime.atmospheres.size(); ++i)
		{
			auto const& a = runtime.atmospheres[i];
			runtime.prediction.include(ProtectionPrediction::Body::Atmosphere, i,
				protectionAtmosphereStart[i], a.temperatureC, a.authored.thermalCapacityKJPerK, seconds,
				{a.lastOccupantHeatKW, a.lastCrewHeatKW, a.lastCropHeatKW, a.lastAlgaeHeatKW,
				 a.lastLatentHeatKW, a.lastExchangeIntoCompartmentKW});
		}
	}

	bool StationSystemsRuntime::Impl::Tick::requireFinite(double value, std::string const& what)
	{
		if (std::isfinite(value)) return true;
		violation = "non-finite " + what + " is not a publishable physical result";
		return false;
	}
}
