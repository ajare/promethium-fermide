#pragma once
#include "core/World.h"
#include "core/StationPorts.h"
#include "core/StationReference.h"
#include "core/StationSystems.h"
#include "core/YamlSerializer.h"
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace smoke::station
{
	inline void require(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }
	// Station scenarios state durations in simulated seconds. The conversion stays
	// at this fixture boundary so check bodies do not encode World tick counts.
	inline bool advanceSeconds(core::World& world, double seconds)
	{
		if (!std::isfinite(seconds) || seconds < 0.0) return false;
		auto const ticks = static_cast<uint64_t>(std::llround(seconds / world.getFixedTimestep()));
		return std::abs(static_cast<double>(ticks) * world.getFixedTimestep() - seconds)
			<= 0.5 * world.getFixedTimestep() && world.advanceTicks(ticks);
	}

	template<class WorldPointer>
	inline bool advanceSeconds(WorldPointer const& world, double seconds)
		requires requires { *world; }
	{
		return advanceSeconds(*world, seconds);
	}

	inline void enable(core::World& world)
	{
		world.pauseSimulation();
		world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems({true, 1}, false, &diagnostic), diagnostic.c_str());
	}

	inline std::string yaml(core::World const& world)
	{
		auto serializer = core::YamlSerializer::toString(); core::SerializationWorkData data;
		world.serialize(*serializer, data); serializer->serialize(); return serializer->getSerializedString();
	}

	struct SourceScope { ~SourceScope() { core::setStationSystemsSourceLoader({}); } };

	inline void danglingSiblingLinks()
	{
		// #669: use the reconciled reference to exercise every authored family,
		// including growable requirements and dependants of coolant loops.
		auto const base = core::bundledStationReference().settings;
		auto const links = core::stationAuthoredLinks(base);
		size_t checked = 0;
		for (auto const& link : links)
		for (auto const& sibling : links)
		{
			auto owner = [](auto const& entry) {
				return entry.identity.substr(0, entry.identity.rfind(':'));
			};
			if (link.identity == sibling.identity || owner(link) != owner(sibling)) continue;
			auto draft = base;
			require(core::clearStationLink(draft, link.identity), "candidate disconnect failed");
			auto broken = sibling;
			auto const prefix = owner(sibling);
			if (prefix == sibling.from.nodeType + ":" + sibling.from.nodeId)
				broken.to.nodeId = "missing";
			else broken.from.nodeId = "missing";
			require(core::applyStationLink(draft, {broken.from, broken.to}), "broken sibling write failed");
			auto cleared = draft;
			require(core::clearStationLink(cleared, sibling.identity), "sibling disconnect failed");
			for (bool reverse : {false, true})
			{
				core::CandidateLink candidate = reverse ? core::CandidateLink{link.to, link.from}
					: core::CandidateLink{link.from, link.to};
				auto const before = draft;
				auto const expected = core::evaluateStationLink(cleared, candidate);
				auto const actual = core::evaluateStationLink(draft, candidate);
				require(expected.allowed, ("reference reconnect refused: " + link.identity + ": " + expected.reason).c_str());
				require(actual.allowed, ("dangling sibling blocked " + link.identity + " beside "
					+ sibling.identity + ": " + actual.reason).c_str());
				require(draft == before, "link evaluation repaired the actual draft");
				// A new semantic error must remain visible beyond that sibling.
				auto invalid = draft;
				for (auto& store : invalid.stores)
					if ((candidate.from.nodeType == "Store" && store.id == candidate.from.nodeId)
						|| (candidate.to.nodeType == "Store" && store.id == candidate.to.nodeId))
						store.unit = store.unit == "kg" ? "g" : "kg";
				for (auto& reservoir : invalid.thermalReservoirs)
					if ((candidate.from.nodeType == "Reservoir" && reservoir.id == candidate.from.nodeId)
						|| (candidate.to.nodeType == "Reservoir" && reservoir.id == candidate.to.nodeId))
						reservoir.thermalClass = reservoir.thermalClass == "warm" ? "moderate" : "warm";
				auto invalidCleared = invalid;
				core::clearStationLink(invalidCleared, sibling.identity);
				auto const invalidExpected = core::evaluateStationLink(invalidCleared, candidate);
				auto const invalidActual = core::evaluateStationLink(invalid, candidate);
				require(invalidActual.allowed == invalidExpected.allowed,
					("dangling sibling masked new violation: " + link.identity).c_str());
				++checked;
			}
		}
		require(checked > 100, "dangling sibling matrix lost coverage");
	}


	// Shared fixtures for the battery smoke scenarios below. Hoisted out of
	// the former monolithic `batteries` check so each scenario can be its own
	// test without duplicating them.
	inline auto const approx = [](double actual, double expected)
	{ return std::abs(actual - expected) < 1e-3; };
	inline auto sawEvent = [](core::World& world, std::string const& needle)
	{
		for (auto const& event : world.getStationSystemsEvents())
			if (event.message.find(needle) != std::string::npos) return true;
		return false;
	};
	inline auto constantProfile = [](double level)
	{
		core::ProfileSettings profile;
		profile.constantLevel = level;
		return profile;
	};
	inline auto array = [](std::string const& id, double capacityKW, core::ProfileSettings illumination)
	{
		return core::SolarArraySettings{id, capacityKW, std::move(illumination)};
	};
	inline auto load = [](std::string const& id, double demandKW, int priority, double maxCurtailment = 1.0)
	{
		core::EquipmentLoadSettings settings;
		settings.id = id;
		settings.electricalDemandKW = demandKW;
		settings.thermalClass = "moderate";
		settings.coolingDisposition = core::StationCoolingDisposition::Exported;
		settings.priority = priority;
		settings.maxCurtailment = maxCurtailment;
		return settings;
	};
	inline auto battery = [](std::string const& id, double nameplateKWh, double usableKWh,
		double startingStoredKWh, double maxChargeKW, double maxDischargeKW,
		double chargeEfficiency = 1.0, double dischargeEfficiency = 1.0)
	{
		core::BatterySettings settings;
		settings.id = id;
		settings.nameplateKWh = nameplateKWh;
		settings.usableKWh = usableKWh;
		settings.startingStoredKWh = startingStoredKWh;
		settings.maxChargeKW = maxChargeKW;
		settings.maxDischargeKW = maxDischargeKW;
		settings.chargeEfficiency = chargeEfficiency;
		settings.dischargeEfficiency = dischargeEfficiency;
		return settings;
	};
	inline auto configure = [](core::World& world,
		std::vector<core::SolarArraySettings> solar,
		std::vector<core::EquipmentLoadSettings> loads,
		std::vector<core::BatterySettings> batteries)
	{
		std::string diagnostic;
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		settings.solarArrays = std::move(solar);
		settings.equipmentLoads = std::move(loads);
		settings.batteries = std::move(batteries);
		return std::pair(world.configureStationSystems(settings, true, &diagnostic), diagnostic);
	};
	inline auto const tight = [](double actual, double expected)
	{ return std::abs(actual - expected) < 1e-9; };
	inline auto const hourTight = [](double actual, double expected)
	{ return std::abs(actual - expected) < 1e-6; };
	inline auto const kWs = 1.0 / 3600.0; // kWh moved by 1 kW in one second
	inline auto const hourTicks = 216000ULL; // one simulated hour at 60 Hz

}
