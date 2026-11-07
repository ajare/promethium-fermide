#pragma once

#include "core/Edge.h"
#include "core/Window.h"

namespace core
{
	class WindowEdge : public Edge
	{
		friend struct RouteTraversalInputs;
		std::shared_ptr<Window> mWindow;

	public:
		WindowEdge(uint32_t id, std::shared_ptr<Window> window);
		explicit WindowEdge(std::shared_ptr<Window> window);

		std::shared_ptr<Edge> copyWithoutVertices() override;
		std::string getDescription() const override;
		bool isTraversable(std::shared_ptr<const Vertex> targetVertex,
			std::shared_ptr<const Agent> agent) const override;
		EdgeTraversalRequestResult requestTraversal(std::shared_ptr<const Vertex> targetVertex,
			std::shared_ptr<const Agent> agent) const override;
		[[nodiscard]] DirectedTraversalFacts getDirectedTraversalFacts(
			std::shared_ptr<const Vertex> targetVertex,
			RouteDecisionContext const& context) const override;
		TraversalResourceId getTraversalResourceId() const override;
	};
}
