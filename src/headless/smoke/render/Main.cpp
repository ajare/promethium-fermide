#include "Walls.h"
#include "Smoke.h"
#include "AgentPaths.h"
#include "Transports.h"

namespace
{
	void walls(smoke::Context const&)
	{
		runWallRenderSmokeChecks();
	}

	constexpr smoke::Check checks[] = {
		{ "walls", walls },
		{ "agentPaths", agentPaths },
		{ "carriageDoors", carriageDoors },
		{ "carriageImages", carriageImages },
	};
}

int main(int argc, char** argv)
{
	return smoke::main("render", checks, argc, argv);
}
