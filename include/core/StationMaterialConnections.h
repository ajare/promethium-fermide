#pragma once
#include "core/StationConnectionLedger.h"
#include <functional>

namespace core
{
// Adapts the existing process feasibility checks and commits to physical port
// budgets. Process ledgers retain their device-side quantities; the callbacks
// book the additional feed draw and the undelivered product as external losses.
class StationMaterialConnections
{
public:
	using Ledger = std::map<std::string, double>;
	using InputLoss = std::function<void(size_t, double)>;
	using OutputLoss = std::function<void(size_t, double, double, Ledger const&)>;
	StationMaterialConnections(StationConnectionLedger& connections, StationSystemsSettings const& settings,
		std::string const& type, std::string const& id, double seconds,
		std::vector<double> const& in, std::vector<double> const& out,
		std::vector<double> const& qualified, bool& valid,
		std::vector<Ledger> const& drift, InputLoss inputLoss, OutputLoss outputLoss)
		: mConnections(connections), mSeconds(seconds), mIn(in), mOut(out), mQualified(qualified), mSettings(settings), mValid(valid),
		mInputLoss(std::move(inputLoss)), mOutputLoss(std::move(outputLoss))
	{
		for (auto const& binding : connections.storePorts(type, id, settings))
			mRecords.push_back({binding, in[binding.store], out[binding.store], drift[binding.store], {}, 0.0});
	}
	StationMaterialConnections(StationMaterialConnections const&) = delete;
	StationMaterialConnections& operator=(StationMaterialConnections const&) = delete;
	~StationMaterialConnections() { finish(); }
	void used(std::string const& role, std::function<double()> amount)
	{
		for (auto& record : mRecords)
			if (mConnections.role(record.binding.port) == role) record.used = std::move(amount);
	}
	// Feasibility for operation differs from feasibility for requesting an
	// entirely lossy input: only that inlet's zero efficiency is disregarded
	// for demand. Independent output, co-feed, and service limits still apply.
	struct Request
	{
		double available, demandAvailable;
		operator double() const { return available; }
	};
	void limitDemand(double available, double required)
	{
		if (required > 0.0) mDemandScale = std::min(mDemandScale, std::clamp(available / required, 0.0, 1.0));
	}
	void limitDemand(Request const& request, double required) { limitDemand(request.demandAvailable, required); }
	double demandScale() const { return mDemandScale; }
	void demand(size_t store, double amount, bool independent = false)
	{
		if (amount <= 0.0) return;
		for (auto& record : mRecords)
			if (record.binding.store == store && record.binding.input)
			{
				record.demand = std::max(record.demand, amount);
				record.independent = record.independent || independent;
			}
	}
	void demand(std::string const& role, double amount)
	{
		if (amount <= 0.0) return;
		for (auto& record : mRecords)
			if (mConnections.role(record.binding.port) == role) record.demand = std::max(record.demand, amount);
	}
	Request request(std::string const& role, double available, double requested)
	{
		demand(role, requested);
		for (auto const& record : mRecords)
			if (mConnections.role(record.binding.port) == role)
				return {available, mConnections.efficiency(record.binding.port) == 0.0 ? record.demandAvailable : available};
		return {available, available};
	}
	Request request(size_t store, double available, double requested)
	{
		demand(store, requested);
		for (auto const& record : mRecords)
			if (record.binding.store == store && record.binding.input)
				return {available, mConnections.efficiency(record.binding.port) == 0.0 ? record.demandAvailable : available};
		return {available, available};
	}
	double input(size_t store, double drawable, double requested = 0.0)
	{
		demand(store, requested);
		for (auto& record : mRecords)
			if (record.binding.store == store && record.binding.input) return input(record, drawable);
		return drawable;
	}
	double input(std::string const& role, double drawable, double requested = 0.0)
	{
		for (auto& record : mRecords)
			if (mConnections.role(record.binding.port) == role)
			{
				record.demand = std::max(record.demand, requested);
				return input(record, drawable);
			}
		return drawable;
	}
	double output(size_t store, double headroom) const
	{
		for (auto const& record : mRecords)
			if (record.binding.store == store && !record.binding.input)
			{
				auto const produced = amount(record);
				auto const eta = mConnections.efficiency(record.binding.port);
				auto const room = headroom + produced * (1.0 - eta);
				auto const capacity = std::max(0.0, mConnections.remaining(record.binding.port) - produced / mSeconds) * mSeconds;
				return room <= 0.0 ? 0.0 : std::min(capacity, eta > 0.0 ? room / eta : capacity);
			}
		return headroom;
	}
	void finish()
	{
		if (mFinished) return;
		mFinished = true;
		for (auto const& record : mRecords)
		{
			auto const used = amount(record);
			if (record.binding.input)
			{
				auto const request = mConnections.efficiency(record.binding.port) == 0.0
					? record.demand * (record.independent ? 1.0 : mDemandScale) : used;
				auto const available = std::max(0.0, mQualified[record.binding.store]
					- mSettings.stores[record.binding.store].protectedReserve) + used;
				auto const transport = mConnections.receive(record.binding.port, request / mSeconds, available / mSeconds);
				if (used > transport.delivered * mSeconds + 1e-8 * std::max(1.0, used)) mValid = false;
				mInputLoss(record.binding.store, std::max(0.0, transport.input * mSeconds - used));
			}
			else
			{
				auto const transport = mConnections.send(record.binding.port, used / mSeconds);
				if (used > transport.input * mSeconds + 1e-8 * std::max(1.0, used)) mValid = false;
				mOutputLoss(record.binding.store, used, std::max(0.0, used - transport.delivered * mSeconds), record.drift);
			}
		}
	}
private:
	struct Record
	{
		StationConnectionLedger::StorePort binding;
		double in, out;
		Ledger drift;
		std::function<double()> used;
		double demand;
		double demandAvailable{0.0};
		bool independent{false};
	};
	double amount(Record const& record) const
	{
		return std::max(0.0, record.used ? record.used() : record.binding.input
			? mOut[record.binding.store] - record.out : mIn[record.binding.store] - record.in);
	}
	double input(Record& record, double drawable) const
	{
		auto const consumed = amount(record);
		auto const port = record.binding.port;
		auto const grossRate = mConnections.required(port, consumed / mSeconds);
		double pendingLoss = 0.0;
		for (auto const& other : mRecords)
			if (other.binding.input && other.binding.store == record.binding.store)
			{
				auto const used = amount(other);
				pendingLoss += std::max(0.0, mConnections.required(other.binding.port, used / mSeconds) * mSeconds - used);
			}
		auto const remaining = std::max(0.0, mConnections.remaining(port) - grossRate);
		auto const rate = std::min(std::max(0.0, drawable - pendingLoss) / mSeconds, remaining);
		record.demandAvailable = rate * mSeconds;
		return rate * mConnections.efficiency(port) * mSeconds;
	}
	StationConnectionLedger& mConnections;
	double mSeconds;
	std::vector<double> const& mIn;
	std::vector<double> const& mOut;
	std::vector<double> const& mQualified;
	StationSystemsSettings const& mSettings;
	bool& mValid;
	InputLoss mInputLoss;
	OutputLoss mOutputLoss;
	std::vector<Record> mRecords;
	double mDemandScale{1.0};
	bool mFinished{};
};
}
