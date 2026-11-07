#pragma once
#include "core/Simulation.h"
#include <bit>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace smoke
{
	struct ScenarioResult
	{
		bool reachedDestination{ false };
		core::SimulationSnapshot snapshot;
		std::vector<core::SimulationEvent> events;
	};

	inline void appendAgent(std::ostringstream& output, core::AgentSnapshot const& agent)
	{
		output << agent.id.value << ':' << agent.name << ':' << agent.sectorId.value << ':'
			<< std::bit_cast<uint32_t>(agent.localPosition.x) << ':'
			<< std::bit_cast<uint32_t>(agent.localPosition.y) << ':'
			<< std::bit_cast<uint32_t>(agent.globalPosition.x) << ':'
			<< std::bit_cast<uint32_t>(agent.globalPosition.y) << ':'
			<< (int)agent.state << ':' << agent.hasPath << ':'
			<< agent.targetPathNode << ':' << agent.pathNodeCount;
	}

	inline void appendTraversalRequest(std::ostringstream& output, core::TraversalRequestSnapshot const& request)
	{
		output << request.id.value << ':' << request.owner.value << ':' << (int)request.edgeType << ':'
			<< request.sourceSector.value << ':' << request.destinationSector.value << ':'
			<< std::bit_cast<uint32_t>(request.sourceEndpoint.x) << ':'
			<< std::bit_cast<uint32_t>(request.sourceEndpoint.y) << ':'
			<< std::bit_cast<uint32_t>(request.destinationEndpoint.x) << ':'
			<< std::bit_cast<uint32_t>(request.destinationEndpoint.y) << ':'
			<< (int)request.state << ':' << request.permit.value << ':' << request.diagnostic;
	}

	inline std::string canonicalResult(ScenarioResult const& result)
	{
		std::ostringstream output;
		output << result.snapshot.tick << '|';
		for (auto const& agent : result.snapshot.agents)
		{
			appendAgent(output, agent);
			output << '|';
		}

		for (auto const& event : result.events)
		{
			output << event.sequence << ':' << event.tick << ':' << (int)event.type << ':'
				<< (int)event.phase << ':' << event.hasPreviousAgent << ':';
			if (event.hasPreviousAgent)
			{
				appendAgent(output, event.previousAgent);
			}
			output << ':';
			switch (event.type)
			{
			case core::SimulationEventType::AgentAdded:
			case core::SimulationEventType::AgentChanged:
			case core::SimulationEventType::AgentActivated:
			case core::SimulationEventType::AgentDeactivated:
			case core::SimulationEventType::AgentRemoved:
				appendAgent(output, event.agent);
				break;
			case core::SimulationEventType::TraversalRequestAdded:
			case core::SimulationEventType::TraversalRequestChanged:
			case core::SimulationEventType::TraversalRequestRemoved:
				appendTraversalRequest(output, event.traversalRequest);
				break;
			case core::SimulationEventType::TraversalPermitAdded:
			case core::SimulationEventType::TraversalPermitChanged:
			case core::SimulationEventType::TraversalPermitRemoved:
				output << event.traversalPermit.id.value << ':' << event.traversalPermit.request.value
					<< ':' << event.traversalPermit.owner.value << ':' << (int)event.traversalPermit.state;
				break;
			default:
				break;
			}
			output << '|';
		}
		return output.str();
	}
}
