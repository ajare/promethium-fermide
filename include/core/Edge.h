#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/EdgeType.h"
#include "core/RouteCost.h"
#include "core/EdgeTraversalRequestResult.h"
#include "core/EntityId.h"


namespace core
{
	class Location;
	class Vertex;
	class Agent;

	class Edge
	{
		friend class Graph;
		friend class Agent;

	private:

		static uint32_t IdGenerator;

	private:

		uint32_t mId;
		uint32_t mRoutingIndex = 0;

		EdgeType mType;
		int mLocalDepth{ 0 };

		// Endpoint Vertices are owned by the owning Graph (and, transitively, by
		// its World).  They are referenced weakly so the Vertex <-> Edge adjacency
		// does not form a strong-reference cycle that outlives the Graph (#182).
		std::weak_ptr<const Vertex> mVertices[2];
		// Geometry is fixed when Graph construction attaches both endpoints.
		float mLength = 0;
		float mRise = 0;
		uint32_t mTargetVertexId = 0;
		mutable uint32_t mStandingRouteAgents = 0;
		mutable uint64_t mStandingRouteEpoch = 0;
		void changeStandingRouteAgents(int delta) const;

	private:
		
		void _setVertex(uint32_t index, std::shared_ptr<const Vertex> vertex);

	protected:

		Edge(uint32_t id, EdgeType type);

	public:

		Edge(EdgeType type);

		virtual ~Edge() = default;

		// Compares Edges by their ID.
		[[nodiscard]] bool sameAs(std::shared_ptr<const Edge> other) const;

		[[nodiscard]] uint32_t getId() const;
		// Dense Graph-local identity, reproducible after a World reset. Unlike
		// getId(), this is not a process-global object identity.
		[[nodiscard]] uint32_t getRoutingIndex() const { return mRoutingIndex; }

		[[nodiscard]] EdgeType getType() const;

		[[nodiscard]] bool isInterLayer() const;

		[[nodiscard]] std::shared_ptr<const Vertex> getVertex(uint32_t index) const;

		[[nodiscard]] std::shared_ptr<const Vertex> getOtherVertex(std::shared_ptr<const Vertex> vertex) const;

		int getLocalDepth() const { return mLocalDepth; }
		float getLength() const;
		float getDirectedRise(Vertex const& target) const;
		uint32_t getStandingRouteAgents() const { return mStandingRouteAgents; }
		uint64_t getStandingRouteEpoch() const { return mStandingRouteEpoch; }

		// To be implemented by subclasses.
		[[nodiscard]] virtual std::string getDescription() const = 0;

		// To be implemented by subclasses.
		[[nodiscard]] virtual std::shared_ptr<Edge> copyWithoutVertices() = 0;

		// To be implemented by subclasses.
		[[nodiscard]] virtual bool isTraversable(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const = 0;

		// To be implemented by subclasses.
		virtual EdgeTraversalRequestResult requestTraversal(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const = 0 ;

		// Supplies directed feasibility and objective facts for route evaluation.
		[[nodiscard]] virtual DirectedTraversalFacts getDirectedTraversalFacts(
			std::shared_ptr<const Vertex> targetVertex, RouteDecisionContext const& context) const = 0;

		// True when using this Edge necessarily requires operating an interaction
		// point. This is a pure function of authored resource data.
		[[nodiscard]] virtual bool requiresButton() const { return false; }

		// A positive value overrides the Agent's normal locomotion speed while crossing.
		[[nodiscard]] virtual float getTraversalSpeed(Agent const* /* agent */,
			std::shared_ptr<const Vertex> const& /* targetVertex */ = {}) const { return 0.0f; }
	
		// A non-zero handle selects the traversal authority; zero is the explicit
		// immediate-permit policy.
		[[nodiscard]] virtual TraversalResourceId getTraversalResourceId() const { return {}; }
	};

} // core
