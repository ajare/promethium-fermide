#pragma once

#include <memory>
#include <vector>

#include "core/Edge.h"
#include "core/Vertex.h"


namespace core
{
	struct PathNode
	{
		std::shared_ptr<const Edge> edge;
		std::shared_ptr<const Vertex> targetVertex;
		// Legacy name retained for serialization/runtime compatibility; this is a
		// cumulative perceived score, never a movement duration. Inferred-source
		// Paths include the initial walk to their first vertex (whose edge is null).
		float edgeWeight;
		std::optional<float> objectiveDurationSeconds;

		float getCumulativePerceivedCost() const { return edgeWeight; }
	};

	struct Path
	{
		std::vector<PathNode> nodes;
	};

} // core
