#include "core/PhysicalControlPlacement.h"
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace core::physicalControl
{
	using namespace std;

	int64_t Candidate::centreKey() const
	{
		return static_cast<int64_t>(cellX) * 4 + quarterOffset;
	}

	float Candidate::centreX() const
	{
		return cellX + quarterOffset * 0.25f;
	}

	Candidate Candidate::explicitHost(uint32_t x, int offset)
	{
		if (offset < 0 || offset > 3)
			throw invalid_argument("Invalid explicit physical-control candidate");
		return { x, offset };
	}

	bool canonicalLess(Owner const& a, Owner const& b)
	{
		auto key = [](Owner const& owner)
		{
			auto const& g = owner.geometry;
			auto const& h = owner.hostingLocation;
			auto const& r = owner.role;
			return tuple{ g.x, owner.type, g.layer, g.baseLevel, g.width, g.height,
				h.layer, h.x, h.baseLevel, h.width, h.height, r.order, r.x, r.level };
		};
		return key(a) < key(b);
	}

	vector<uint32_t> allocateCanonical(vector<Demand> const& demands)
	{
		vector<uint32_t> order, assignment(demands.size());
		for (uint32_t i = 0; i < demands.size(); ++i)
		{
			if (demands[i].candidates.empty()) throw runtime_error("Physical control requires candidates");
			for (auto const& candidate : demands[i].candidates)
				if (candidate.quarterOffset < 0 || candidate.quarterOffset > 3)
					throw runtime_error("Invalid physical-control candidate");
			order.push_back(i);
		}
		sort(order.begin(), order.end(), [&](auto a, auto b)
		{
			return canonicalLess(demands[a].owner, demands[b].owner);
		});
		for (size_t i = 1; i < order.size(); ++i)
			if (!canonicalLess(demands[order[i]].owner, demands[order[i - 1]].owner)
				&& !canonicalLess(demands[order[i - 1]].owner, demands[order[i]].owner))
				throw runtime_error("Indistinguishable duplicate physical-control definitions");

		auto connected = [&](uint32_t a, uint32_t b)
		{
			for (auto const& x : demands[a].candidates)
				for (auto const& y : demands[b].candidates)
					if (x.centreKey() == y.centreKey()) return true;
			return false;
		};
		// Keep the best assignment at each capacity. The maximum-stack tier is
		// global: a forced four-stack in one disconnected component must not
		// make another component sacrifice preferred sides to reduce its own max.
		struct Solution { vector<uint32_t> choice; pair<uint32_t, uint32_t> score{~0u, ~0u}; };
		struct Component { vector<uint32_t> indices; vector<Solution> capacities; };
		vector<Component> components;
		vector<bool> visited(demands.size());
		for (auto root : order)
		{
			if (visited[root]) continue;
			vector<uint32_t> component{ root };
			visited[root] = true;
			for (size_t i = 0; i < component.size(); ++i)
				for (auto other : order)
					if (!visited[other] && connected(component[i], other))
					{ visited[other] = true; component.push_back(other); }
			// Restore the complete authored ordering after connectivity discovery.
			vector<uint32_t> sorted;
			for (auto index : order)
				if (find(component.begin(), component.end(), index) != component.end()) sorted.push_back(index);
			component = std::move(sorted);

			Component result{component, vector<Solution>(4)};
			for (uint32_t capacity = 1; capacity <= 4; ++capacity)
			{
				auto& best = result.capacities[capacity - 1];
				vector<uint32_t> choice(component.size());
				function<void(uint32_t, uint32_t, uint32_t)> search = [&](uint32_t depth, uint32_t stacked, uint32_t penalty)
				{
					// Both costs are monotone. Preferred-first canonical enumeration
					// resolves exact ties without using previous assignments or IDs.
					auto score = make_pair(stacked, penalty);
					if (!best.choice.empty() && score >= best.score) return;
					if (depth == component.size()) { best.choice = choice; best.score = score; return; }
					auto const& demand = demands[component[depth]];
					vector<uint32_t> candidates;
					for (uint32_t i = 0; i < demand.candidates.size(); ++i) candidates.push_back(i);
					stable_sort(candidates.begin(), candidates.end(), [&](auto a, auto b)
					{
						return (a == demand.defaultCandidate) > (b == demand.defaultCandidate);
					});
					for (auto candidate : candidates)
					{
						auto const& position = demand.candidates[candidate];
						bool collision = false;
						uint32_t coincident = 0;
						for (uint32_t i = 0; i < depth; ++i)
						{
							auto const& other = demands[component[i]];
							auto const& occupied = other.candidates[choice[i]];
							if (position.centreKey() == occupied.centreKey())
							{
								auto const& a = demand.owner.hostingLocation;
								auto const& b = other.owner.hostingLocation;
								collision |= tie(a.layer, a.x, a.baseLevel, a.width, a.height)
									!= tie(b.layer, b.x, b.baseLevel, b.width, b.height);
								++coincident;
							}
						}
						if (collision || coincident >= capacity) continue;
						choice[depth] = candidate;
						// Adding to any occupied position adds ONE button above a bottom,
						// not the number of pairs within the stack.
						search(depth + 1, stacked + (coincident != 0), penalty + (candidate != demand.defaultCandidate));
					}
				};
				search(0, 0, 0);
			}
			if (result.capacities.back().choice.empty())
				throw runtime_error("Physical controls require separation or same-Location stacks of at most four");
			components.push_back(std::move(result));
		}
		// First minimise the additive above-bottom count, then find the smallest
		// common capacity that attains it in every component. Only then minimise
		// non-preferred placements and canonical preference ties.
		uint32_t capacity = 1;
		for (; capacity < 4; ++capacity)
			if (all_of(components.begin(), components.end(), [&](auto const& component)
			{
				auto const& solution = component.capacities[capacity - 1];
				return !solution.choice.empty() && solution.score.first == component.capacities.back().score.first;
			})) break;
		for (auto const& component : components)
			for (size_t i = 0; i < component.indices.size(); ++i)
				assignment[component.indices[i]] = component.capacities[capacity - 1].choice[i];
		return assignment;
	}
}
