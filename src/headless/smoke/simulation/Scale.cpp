#include "Checks.h"
#include "PathFixture.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <bit>
#include <set>
#include <vector>
#include "core/SectorEdge.h"
#include "core/SimulationMetricsCollector.h"

namespace
{
	using smoke::twoNodePath;

	struct ScaleObservation
	{
		bool valid{ false };
		uint64_t eventDigest{ 1469598103934665603ull };
		uint64_t snapshotDigest{ 1469598103934665603ull };
	};

	void digestValue(uint64_t& digest, uint64_t value)
	{
		for (uint32_t byte = 0; byte < 8; ++byte)
		{
			digest ^= (value >> (byte * 8)) & 0xffu;
			digest *= 1099511628211ull;
		}
	}

	ScaleObservation runScaledWorld(uint32_t agentCount, uint64_t ticks, bool metricsEnabled = false)
	{
		constexpr uint32_t ResourceCount = 32;
		core::World world("Scale world", 80, 2);
		auto corridor = world.addCorridor(0, 0, 79);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(corridor, 0, 70.5f, &destinationVertexId);
		world.finishBuild();
		for (uint32_t i = 0; i < ResourceCount; ++i)
		{
			world.createTraversalResource("Scale resource " + std::to_string(i));
		}

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto edge = std::make_shared<core::SectorEdge>();
		for (uint32_t i = 0; i < agentCount; ++i)
		{
			auto id = world.createAgent("Scale agent " + std::to_string(i), corridor, 0, 0.5f);
			world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		}

		core::SimulationMetricsCollector metrics(world);
		if (metricsEnabled) world.setSimulationObserver(&metrics);
		ScaleObservation result;
		auto digestEvents = [&](std::vector<core::SimulationEvent> const& events)
		{
			for (auto const& event : events)
			{
				digestValue(result.eventDigest, event.sequence);
				digestValue(result.eventDigest, event.tick);
				digestValue(result.eventDigest, (uint64_t)event.type);
				digestValue(result.eventDigest, (uint64_t)event.phase);
				digestValue(result.eventDigest, event.agent.id.value);
				digestValue(result.eventDigest, event.traversalRequest.id.value);
				digestValue(result.eventDigest, event.traversalPermit.id.value);
			}
		};
		digestEvents(world.consumeSimulationEvents());
		for (uint64_t tick = 0; tick < ticks; ++tick)
		{
			world.advanceTick();
			// Event delivery is intentionally incremental in a long-running host.
			if ((tick + 1) % 10 == 0) digestEvents(world.consumeSimulationEvents());
		}
		digestEvents(world.consumeSimulationEvents());

		auto snapshot = world.getSimulationSnapshot();
		std::set<uint64_t> requestOwners;
		result.valid = snapshot.tick == ticks && snapshot.agents.size() == agentCount
			&& snapshot.traversalResources.size() == ResourceCount;
		digestValue(result.snapshotDigest, snapshot.tick);
		digestValue(result.snapshotDigest, snapshot.agents.size());
		digestValue(result.snapshotDigest, snapshot.traversalResources.size());
		for (auto const& agent : snapshot.agents)
		{
			result.valid = result.valid && agent.hasPath && agent.globalPosition.x > 0.5f;
			digestValue(result.snapshotDigest, agent.id.value);
			digestValue(result.snapshotDigest, agent.sectorId.value);
			digestValue(result.snapshotDigest, std::bit_cast<uint32_t>(agent.globalPosition.x));
			digestValue(result.snapshotDigest, std::bit_cast<uint32_t>(agent.globalPosition.y));
			digestValue(result.snapshotDigest, (uint64_t)agent.state);
			digestValue(result.snapshotDigest, agent.hasPath);
			digestValue(result.snapshotDigest, agent.targetPathNode);
			digestValue(result.snapshotDigest, agent.pathNodeCount);
		}
		for (auto const& request : snapshot.traversalRequests)
		{
			result.valid = result.valid && request.state != core::TraversalRequestState::Cancelled
				&& request.state != core::TraversalRequestState::Denied
				&& !request.diagnostic.empty() && requestOwners.insert(request.owner.value).second;
		}
		for (auto const& resource : snapshot.traversalResources)
		{
			result.valid = result.valid
				&& resource.occupantCount + resource.admissionReservationCount <= resource.capacity
				&& resource.virtualBoundaryCrossingCount <= resource.capacity;
			for (auto const& carriage : resource.shuttleCarriages)
				result.valid = result.valid && carriage.occupantCount
					+ carriage.admissionReservationCount <= carriage.capacity;
		}
		return result;
	}
}

void registerScale(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "runScaledWorld", [](smoke::Context const&)
		{
			auto representative = runScaledWorld(500, 60);
			auto repeatedRepresentative = runScaledWorld(500, 60, true);
			smoke::require(representative.valid && repeatedRepresentative.valid
				&& representative.eventDigest == repeatedRepresentative.eventDigest
				&& representative.snapshotDigest == repeatedRepresentative.snapshotDigest,
				"representative scale run violated ownership, capacity, or determinism");
			auto stretch = runScaledWorld(1000, 60);
			smoke::require(stretch.valid, "1,000-agent stretch run violated ownership or capacity");
		} });
}
