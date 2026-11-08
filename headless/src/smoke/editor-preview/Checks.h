#pragma once

#include "Smoke.h"

#include <sstream>
#include <string>
#include <vector>

#include "core/Simulation.h"

namespace editor_preview_smoke
{
	// A stable rendering of everything a run produced, in the same shape the
	// game-runtime module uses, so a preview driven the editor's way and a
	// runtime driven the game's way can be compared as text (#971).
	inline std::string canonical(core::SimulationSnapshot const& snapshot)
	{
		std::ostringstream out;
		out << "tick=" << snapshot.tick << " paused=" << snapshot.paused
			<< " topology=" << snapshot.topologyGeneration << '/'
			<< snapshot.topologyValid << '\n';
		for (auto const& agent : snapshot.agents)
			out << agent.id.value << '|' << agent.name << '|' << agent.sectorId.value
				<< '|' << agent.localPosition.x << '|' << agent.localPosition.y << '|'
				<< agent.globalPosition.x << '|' << agent.globalPosition.y << '|'
				<< static_cast<int>(agent.state) << '|' << agent.hasPath << '\n';
		return out.str();
	}

	inline std::string digest(std::vector<core::SimulationEvent> const& events)
	{
		std::ostringstream out;
		for (auto const& event : events)
			out << event.sequence << ':' << static_cast<int>(event.type) << ':'
				<< static_cast<int>(event.phase) << ':' << event.agent.id.value << ':'
				<< event.agent.globalPosition.x << '\n';
		return out.str();
	}
}

void registerEditorPreviewDocument(std::vector<smoke::Check>& checks);
void registerEditorPreviewParity(std::vector<smoke::Check>& checks);
void registerEditorPreviewBoundary(std::vector<smoke::Check>& checks);
