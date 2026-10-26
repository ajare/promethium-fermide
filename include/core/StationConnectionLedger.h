#pragma once
#include "core/StationConnections.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string_view>
#include <tuple>

namespace core
{
// Tick-local transport budgets. Call only from the authoritative allocator;
// publish results after the inventory/energy audits have accepted the tick.
class StationConnectionLedger
{
public:
	struct Flow { double input{}, delivered{}; };
	StationConnectionLedger(StationSystemsSettings const& settings,
		station_connections::Graph const& graph, double seconds,
		std::vector<StationConnectionSnapshot> const& previous) : mSeconds(seconds)
	{
		mEntries.reserve(graph.links.size());
		for (auto const& link : graph.links)
		{
			if (station_connections::automaticConnectionType(graph, link) == StationPhysicalConnection::NonPhysical) continue;
			Entry entry;
			entry.input = graph.nodes[link.from].type == link.designKey.peerType
				&& graph.nodes[link.from].id == link.designKey.peerId;
			entry.properties = station_connections::connectionProperties(settings, link);
			entry.snapshot.key = link.designKey;
			entry.snapshot.rateUnit = station_connections::connectionThroughputUnit(graph, link, settings);
			// Runtime topology is stable: try the same slot, but always check
			// the full identity. Reordered/missing accounts still match by key.
			auto const slot = mEntries.size();
			auto const found = slot < previous.size() && previous[slot].key == link.designKey
				? previous.begin() + slot
				: std::find_if(previous.begin(), previous.end(), [&](auto const& item) { return item.key == link.designKey; });
			if (found != previous.end())
			{
				entry.snapshot.inputTotal = found->inputTotal;
				entry.snapshot.deliveredTotal = found->deliveredTotal;
				entry.snapshot.lostTotal = found->lostTotal;
				entry.snapshot.retainedHeatKJ = found->retainedHeatKJ;
				entry.snapshot.producedHeatKJ = found->producedHeatKJ;
				entry.snapshot.heatProducer = found->heatProducer;
			}
			mLookup.emplace(std::tuple{link.designKey.ownerType, link.designKey.ownerId, link.designKey.relation}, mEntries.size());
			// #883: a Shared-supply consumer holds one connection per bus it draws
			// from, all under the same relation. The peer-scoped index keeps every
			// one of those cables addressable, so a pro-rata draw books its share of
			// throughput and loss against the cable it really travelled.
			mPeerLookup.emplace(std::tuple{link.designKey.ownerType, link.designKey.ownerId,
				link.designKey.relation, link.designKey.peerId}, mEntries.size());
			mEntries.push_back(std::move(entry));
		}
	}
	using Port = size_t;
	struct StorePort { Port port; size_t store; bool input; };
	std::vector<StorePort> storePorts(std::string const& type, std::string const& id,
		StationSystemsSettings const& settings) const
	{
		std::vector<StorePort> result;
		for (size_t i = 0; i < mEntries.size(); ++i)
		{
			auto const& key = mEntries[i].snapshot.key;
			if (key.ownerType != type || key.ownerId != id || key.peerType != "Store") continue;
			for (size_t j = 0; j < settings.stores.size(); ++j)
				if (settings.stores[j].id == key.peerId)
					result.push_back({i, j, mEntries[i].input});
		}
		return result;
	}
	std::string const& role(Port port) const { return mEntries[port].snapshot.key.relation; }
	StationConnectionSnapshot const* snapshot(Port port) const
	{
		return port == Missing ? nullptr : &mEntries[port].snapshot;
	}
	static constexpr Port Missing = std::numeric_limits<Port>::max();
	Port port(std::string_view type, std::string_view id, std::string_view role) const
	{
		auto found = mLookup.find(std::tuple{type, id, role});
		return found == mLookup.end() ? Missing : found->second;
	}
	// The account for one role against one specific peer. Used where a member
	// is wired to several buses at once and each wire is its own transport.
	Port port(std::string_view type, std::string_view id, std::string_view role,
		std::string_view peerId) const
	{
		auto found = mPeerLookup.find(std::tuple{type, id, role, peerId});
		return found == mPeerLookup.end() ? Missing : found->second;
	}
	double efficiency(Port port) const { return port == Missing ? 1.0 : mEntries[port].properties.efficiency; }
	double remaining(Port port) const
	{
		return port == Missing ? std::numeric_limits<double>::max()
			: std::max(0.0, mEntries[port].properties.maximumThroughput - mEntries[port].snapshot.lastInputRate);
	}
	double available(Port port, double inputRate) const
	{
		return std::min(std::max(0.0, inputRate), remaining(port)) * efficiency(port);
	}
	// Receiver demand is grossed up BEFORE the input-side budget is applied.
	// At zero efficiency an active request still sends input, all of it lost.
	double required(Port port, double deliveredRate) const
	{
		if (deliveredRate <= 0.0) return 0.0;
		auto const eta = efficiency(port);
		return std::min(remaining(port), eta > 0.0 ? deliveredRate / eta : deliveredRate);
	}
	// A next-tick request uses the full rating, not this tick's remainder.
	double inputDemand(Port port, double deliveredRate) const
	{
		if (deliveredRate <= 0.0) return 0.0;
		auto const eta = efficiency(port);
		auto const cap = port == Missing ? std::numeric_limits<double>::max() : mEntries[port].properties.maximumThroughput;
		return std::min(cap, eta > 0.0 ? deliveredRate / eta : deliveredRate);
	}
	Flow receive(Port port, double deliveredRequest, double availableInput)
	{
		return send(port, std::min(required(port, deliveredRequest), std::max(0.0, availableInput)));
	}
	Flow send(Port port, double offeredInput)
	{
		auto const input = std::min(remaining(port), std::max(0.0, offeredInput));
		auto const delivered = input * efficiency(port);
		if (port != Missing)
		{
			auto& value = mEntries[port].snapshot;
			value.lastInputRate += input;
			value.lastDeliveredRate += delivered;
			value.lastLossRate += input - delivered;
			value.inputTotal += input * mSeconds;
			value.deliveredTotal += delivered * mSeconds;
			value.lostTotal += (input - delivered) * mSeconds;
		}
		return {input, delivered};
	}
	void direction(Port port, bool reversed)
	{
		if (port != Missing) mEntries[port].snapshot.reversed = reversed;
	}
	// Material doses are already quantities. Preserve a binding amount
	// exactly instead of rounding it through amount / seconds * seconds.
	Flow sendAmount(Port port, double offeredAmount)
	{
		if (mSeconds <= 0.0) return {0.0, 0.0};
		auto const input = std::min(remaining(port) * mSeconds, std::max(0.0, offeredAmount));
		auto const delivered = input * efficiency(port);
		if (port != Missing)
		{
			auto& value = mEntries[port].snapshot;
			value.lastInputRate += input / mSeconds;
			value.lastDeliveredRate += delivered / mSeconds;
			value.lastLossRate += (input - delivered) / mSeconds;
			value.inputTotal += input;
			value.deliveredTotal += delivered;
			value.lostTotal += input - delivered;
		}
		return {input, delivered};
	}
	Flow receiveAmount(Port port, double deliveredRequest, double availableInput)
	{
		auto const available = std::min(remaining(port) * mSeconds, std::max(0.0, availableInput));
		auto const eta = efficiency(port);
		auto const required = deliveredRequest <= 0.0 ? 0.0
			: eta > 0.0 ? (deliveredRequest >= available * eta ? available : deliveredRequest / eta)
			: deliveredRequest;
		return sendAmount(port, std::min(available, required));
	}
	double routeHeat(Port port, double producedKW)
	{
		if (port == Missing) return producedKW;
		auto& value = mEntries[port].snapshot;
		value.heatProducer = true;
		value.producedHeatKJ += producedKW * mSeconds;
		auto const energy = value.retainedHeatKJ + producedKW * mSeconds;
		auto const flow = send(port, energy / mSeconds);
		value.retainedHeatKJ = std::max(0.0, energy - flow.input * mSeconds);
		return flow.delivered;
	}
	std::vector<std::pair<StationConnectionKey, double>> drainHeat()
	{
		std::vector<std::pair<StationConnectionKey, double>> result;
		for (size_t i = 0; i < mEntries.size(); ++i)
			if (mEntries[i].snapshot.heatProducer)
			{
				auto const delivered = routeHeat(i, 0.0);
				if (delivered > 0.0) result.push_back({mEntries[i].snapshot.key, delivered});
			}
		return result;
	}
	void refund(Port port, double input)
	{
		if (port == Missing || input <= 0.0) return;
		auto& value = mEntries[port].snapshot;
		input = std::min(input, value.lastInputRate);
		auto const delivered = input * efficiency(port);
		value.lastInputRate -= input; value.lastDeliveredRate -= delivered;
		value.lastLossRate -= input - delivered;
		value.inputTotal -= input * mSeconds; value.deliveredTotal -= delivered * mSeconds;
		value.lostTotal -= (input - delivered) * mSeconds;
	}
	template<class Visitor> void forEachSnapshot(Visitor&& visitor) const
	{
		for (auto const& entry : mEntries) visitor(entry.snapshot);
	}
	std::vector<StationConnectionSnapshot> snapshots() const
	{
		std::vector<StationConnectionSnapshot> result;
		result.reserve(mEntries.size());
		forEachSnapshot([&](auto const& snapshot) { result.push_back(snapshot); });
		return result;
	}
	// Successful tick commit only: move published accounts out of this
	// tick-local ledger after every audit has accepted them.
	std::vector<StationConnectionSnapshot> takeSnapshots()
	{
		std::vector<StationConnectionSnapshot> result;
		result.reserve(mEntries.size());
		for (auto& entry : mEntries) result.push_back(std::move(entry.snapshot));
		return result;
	}
	bool finite() const
	{
		for (auto const& entry : mEntries)
		{
			auto const& v = entry.snapshot;
			for (auto value : {v.lastInputRate, v.lastDeliveredRate, v.lastLossRate,
				v.inputTotal, v.deliveredTotal, v.lostTotal, v.retainedHeatKJ, v.producedHeatKJ})
				if (!std::isfinite(value) || value < -1e-9) return false;
			if (v.heatProducer && std::abs(v.producedHeatKJ - v.inputTotal - v.retainedHeatKJ)
				> 1e-8 * std::max(1.0, v.producedHeatKJ)) return false;
		}
		return true;
	}
private:
	struct Entry { StationConnectionProperties properties; StationConnectionSnapshot snapshot; bool input{}; };
	double mSeconds;
	std::vector<Entry> mEntries;
	std::map<std::tuple<std::string, std::string, std::string>, Port, std::less<>> mLookup;
	std::map<std::tuple<std::string, std::string, std::string, std::string>, Port, std::less<>> mPeerLookup;
};
}
