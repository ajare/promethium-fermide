#include "Checks.h"

#include "game/WorldAuthoring.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	registerGameAuthoringWrapping(checks);
	registerGameAuthoringMutationThroughRuntime(checks);
	registerGameAuthoringSharedLibrary(checks);
	registerGameAuthoringTopology(checks);
	registerGameAuthoringUndoRedo(checks);
	registerGameAuthoringResourceBinding(checks);
	registerGameAuthoringPersistence(checks);
	registerGameAuthoringBoundary(checks);
	registerGameAuthoringFloorDepth(checks);
	registerFloorDepthTypeDefaultsAuthoring(checks);
	return smoke::main("game-authoring", checks, argc, argv);
}
