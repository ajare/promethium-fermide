#pragma once

#include "core/Edge.h"

#include <filesystem>

namespace pause_position
{
	void clearPausedPathDoesNotResume();
	void pauseWalkingAgent(bool startsAtMarker);
	void pauseOnStairsPreservesPosition(std::filesystem::path const& filename,
		core::EdgeType edgeType);
	void runAll();
	void runRepro(char const* filename);
}
