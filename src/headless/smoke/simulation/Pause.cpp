#include "Checks.h"
#include "PausePosition.h"

void registerPause(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "clearPausedPathDoesNotResume", [](smoke::Context const&)
		{
			pause_position::clearPausedPathDoesNotResume();
		} });
	checks.push_back({ "pauseTraversingEdgePreservesPosition", [](smoke::Context const&)
		{
			pause_position::pauseWalkingAgent(true);
		} });
	checks.push_back({ "pauseMovingToVertexPreservesPosition", [](smoke::Context const&)
		{
			pause_position::pauseWalkingAgent(false);
		} });
	checks.push_back({ "pauseOnStaircasePreservesPosition", [](smoke::Context const& context)
		{
			pause_position::pauseOnStairsPreservesPosition(
				context.fixture("resources/test-worlds/staircase-test-1.world.yaml"),
				core::EdgeType::Staircase);
		} });
	checks.push_back({ "pauseOnStairwellPreservesPosition", [](smoke::Context const& context)
		{
			pause_position::pauseOnStairsPreservesPosition(
				context.fixture("resources/test-worlds/stairwell-test-1.world.yaml"),
				core::EdgeType::Stairwell);
		} });
}
