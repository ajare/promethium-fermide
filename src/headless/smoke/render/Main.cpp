#include "Walls.h"
#include "Smoke.h"
#include "AgentPaths.h"

namespace
{
	void walls(smoke::Context const&)
	{
		runWallRenderSmokeChecks();
	}

	constexpr smoke::Check checks[] = {
		{ "walls", walls },
		{ "agentPaths", agentPaths },
	};
}

int main(int argc, char** argv)
{
	return smoke::main("render", checks, argc, argv);
}
