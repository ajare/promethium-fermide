#pragma once

#include "core/Edge.h"
#include "core/BulkheadDoor.h"


namespace core
{
	class Agent;

	class BulkheadDoorEdge : public Edge
	{
		friend struct RouteTraversalInputs;
		std::shared_ptr<BulkheadDoor> mDoor;
		std::shared_ptr<class AirlockTransit> mAirlock;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		BulkheadDoorEdge(uint32_t id, std::shared_ptr<BulkheadDoor> door);

		BulkheadDoorEdge(std::shared_ptr<BulkheadDoor> door,
			std::shared_ptr<class AirlockTransit> airlock = {});

		[[nodiscard]] std::shared_ptr<Edge> copyWithoutVertices() override;

		[[nodiscard]] std::string getDescription() const override;

		[[nodiscard]] bool isTraversable(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const override;

		EdgeTraversalRequestResult requestTraversal(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const override;

		DirectedTraversalFacts getDirectedTraversalFacts(std::shared_ptr<const Vertex> targetVertex,
			RouteDecisionContext const& context) const override;

		[[nodiscard]] bool requiresButton() const override;

		TraversalResourceId getTraversalResourceId() const override;
	};

} // core
