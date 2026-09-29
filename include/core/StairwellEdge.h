#pragma once

#include "core/Edge.h"
#include "core/Stairwell.h"


namespace core
{
	class Agent;

	class StairwellEdge : public Edge
	{
		std::shared_ptr<Stairwell> mStairwell;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		StairwellEdge(uint32_t id, std::shared_ptr<Stairwell> stairwell);

		StairwellEdge(std::shared_ptr<Stairwell> stairwell);

		std::shared_ptr<Edge> copyWithoutVertices() override;

		std::string getDescription() const override;

		bool isTraversable(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const override;

		EdgeTraversalRequestResult requestTraversal(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const override;

		[[nodiscard]] DirectedTraversalFacts getDirectedTraversalFacts(
			std::shared_ptr<const Vertex> targetVertex, RouteDecisionContext const& context) const override;

		[[nodiscard]] float getTraversalSpeed(Agent const* agent,
			std::shared_ptr<const Vertex> const& targetVertex = {}) const override;

		TraversalResourceId getTraversalResourceId() const override;
	};

} // core
