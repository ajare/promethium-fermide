#include "core/PhysicalControlPlacement.h"
#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>
#include <utility>

namespace core::physicalControl
{
	using namespace std;

	int64_t Candidate::centreKey() const
	{
		return static_cast<int64_t>(cellX) * 4 + (quarterOffset >= 0
			? quarterOffset : side == CORE_SIDE_RIGHT ? 4 : side == CORE_SIDE_MIDDLE ? 2 : 0);
	}

	float Candidate::centreX() const
	{
		return cellX + (quarterOffset >= 0 ? quarterOffset * 0.25f
			: side == CORE_SIDE_RIGHT ? 1.0f : side == CORE_SIDE_MIDDLE ? 0.5f : 0.0f);
	}

	Candidate Candidate::explicitHost(uint32_t x, int offset, int registrationSide)
	{
		if (offset < 0 || offset > 3 || registrationSide < CORE_SIDE_LEFT
			|| registrationSide > CORE_SIDE_MIDDLE)
			throw invalid_argument("Invalid explicit physical-control candidate");
		return { x, registrationSide, offset };
	}

	vector<Candidate> legacyCandidates(uint32_t x, int side, uint32_t alternateX, int alternateSide)
	{
		vector<Candidate> candidates{ { x, side } };
		if (alternateX != ~0u && alternateSide >= CORE_SIDE_LEFT
			&& alternateSide <= CORE_SIDE_MIDDLE && (alternateX != x || alternateSide != side))
			candidates.push_back({ alternateX, alternateSide });
		return candidates;
	}

	vector<uint32_t> allocateLegacy(vector<Demand> const& demands)
	{
		vector<uint32_t> row, assignment;
		for (uint32_t i = 0; i < demands.size(); ++i)
		{
			row.push_back(i);
			assignment.push_back(demands[i].currentCandidate);
		}
		auto centerKey = [](Candidate const& candidate) { return candidate.centreKey(); };
		auto connected = [&](uint32_t left, uint32_t right)
		{
			for (auto const& a : demands[left].candidates)
				for (auto const& b : demands[right].candidates)
					if (centerKey(a) == centerKey(b)) return true;
			return false;
		};

		vector<bool> visited(row.size(), false);
		for (uint32_t root = 0; root < row.size(); ++root)
		{
			if (visited[root]) continue;
			vector<uint32_t> component;
			vector<uint32_t> pending{ root };
			visited[root] = true;
			while (!pending.empty())
			{
				auto local = pending.back();
				pending.pop_back();
				component.push_back(row[local]);
				for (uint32_t other = 0; other < row.size(); ++other)
				{
					if (!visited[other] && connected(row[local], row[other]))
					{
						visited[other] = true;
						pending.push_back(other);
					}
				}
			}
			sort(component.begin(), component.end());

			vector<uint32_t> choice(component.size()), bestChoice;
			set<pair<uint32_t, int>> occupiedSlots;
			bool haveBest = false;
			uint32_t bestUnique = 0, bestMoved = 0, bestDefaults = 0;
			function<void(uint32_t)> search = [&](uint32_t depth)
			{
				if (depth != component.size())
				{
					auto const& placement = demands[component[depth]];
					for (uint32_t candidateIndex = 0; candidateIndex < placement.candidates.size(); ++candidateIndex)
					{
						auto const& candidate = placement.candidates[candidateIndex];
						auto slot = make_pair(candidate.cellX, candidate.side);
						if (!occupiedSlots.insert(slot).second) continue;
						choice[depth] = candidateIndex;
						search(depth + 1);
						occupiedSlots.erase(slot);
					}
					return;
				}

				set<int64_t> centers;
				uint32_t moved = 0, defaults = 0;
				for (uint32_t i = 0; i < component.size(); ++i)
				{
					auto const& placement = demands[component[i]];
					centers.insert(centerKey(placement.candidates[choice[i]]));
					moved += choice[i] != placement.currentCandidate;
					defaults += choice[i] == placement.defaultCandidate;
				}
				auto unique = static_cast<uint32_t>(centers.size());
				bool better = !haveBest || unique > bestUnique
					|| (unique == bestUnique && moved < bestMoved);
				if (!better && haveBest && unique == bestUnique && moved == bestMoved)
				{
					for (uint32_t i = 0; i < component.size(); ++i)
					{
						auto const& placement = demands[component[i]];
						bool retained = choice[i] == placement.currentCandidate;
						bool bestRetained = bestChoice[i] == placement.currentCandidate;
						if (retained != bestRetained) { better = retained; break; }
					}
					if (!better)
					{
						bool sameRetention = true;
						for (uint32_t i = 0; i < component.size(); ++i)
						{
							auto const& placement = demands[component[i]];
							if ((choice[i] == placement.currentCandidate)
								!= (bestChoice[i] == placement.currentCandidate))
							{ sameRetention = false; break; }
						}
						if (sameRetention && (defaults > bestDefaults
							|| (defaults == bestDefaults && choice < bestChoice))) better = true;
					}
				}
				if (better)
				{
					haveBest = true;
					bestUnique = unique;
					bestMoved = moved;
					bestDefaults = defaults;
					bestChoice = choice;
				}
			};
			search(0);
			if (!haveBest) throw std::runtime_error("Physical-control placement constraints cannot be satisfied");
			for (uint32_t i = 0; i < component.size(); ++i)
				assignment[component[i]] = bestChoice[i];
		}

		return assignment;
	}
}
