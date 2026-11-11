#include "Checks.h"
#include "State.h"

#include "EditorDocument.h"
#include "core/World.h"
#include "game/WorldRuntime.h"

#include <memory>
#include <string>
#include <vector>

// The World panel's four artwork combos bind straight to the World's four
// atlas selections - there is no panel-side copy to drift - so what a fresh
// World carries through the editor's own new-World seam is what the panel
// shows (#1023). This drives that seam rather than the ImGui widgets: the
// widgets have no state of their own to be wrong.
//
// Since the #1013 terminus dropped the `New` suffix, the four sets a fresh
// World shows are the canonical four, and there is no second family left to
// mix with them. What is still worth pinning is that the four slots stay
// independent: repointing one must move that one bound read and no other.

namespace
{
	using smoke::require;

	// Closes whatever document the check opened, so a later check in the same
	// process never reads a World this one left behind.
	struct CloseOnExit
	{
		~CloseOnExit() { editor::closeDocument(); }
	};

	std::vector<std::string> panelSelections(core::World const& world)
	{
		return { world.sectorImageSetResourceName(), world.objectImageSetResourceName(),
			world.agentImageSetResourceName(), world.furnitureImageSetResourceName() };
	}
}

namespace editor_smoke
{
	void registerWorldArtworkPanel(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "world-artwork/newDefaultsShowOnAFreshWorld", [](smoke::Context const&)
			{
				State state;
				CloseOnExit const closer;
				auto const world = editor::openNewWorld("Fresh artwork panel", 8, 2);
				require(world != nullptr, "the editor opened no World");
				auto const shown = panelSelections(*world);
				std::vector<std::string> const expected{
					"SectorAtlas", "ObjectAtlas", "AgentAtlas", "FurnitureAtlas" };
				require(shown == expected,
					"a fresh World's artwork panel does not show the canonical sets: "
					+ shown[0] + ", " + shown[1] + ", " + shown[2] + ", " + shown[3]);
				// The defaults are the World's own initial state rather than an
				// edit made over it: saving the fresh World leaves the panel
				// showing the same four sets, with nothing outstanding.
				world->markSaved();
				require(!world->isModified(), "a saved World arrived dirty before any edit");
				require(panelSelections(*world) == expected,
					"marking a fresh World saved changed the artwork the panel shows");
				// And the runtime behind the panel reads the same four, which is
				// what the viewport draws.
				auto const& runtime = editor::currentDocument()->runtime();
				require(runtime.sectorImageSetResourceName() == "SectorAtlas"
					&& runtime.objectImageSetResourceName() == "ObjectAtlas"
					&& runtime.agentImageSetResourceName() == "AgentAtlas"
					&& runtime.furnitureImageSetResourceName() == "FurnitureAtlas",
					"the runtime behind the artwork panel reads different sets than the panel shows");
			} });

		checks.push_back({ "world-artwork/repointedAtlasMovesItsOwnSlotOnly", [](smoke::Context const&)
			{
				State state;
				CloseOnExit const closer;
				auto const world = editor::openNewWorld("Repointed artwork panel", 8, 2);
				require(world != nullptr, "the editor opened no World");
				// The four selections are four separate writes. Repointing one
				// at a custom theme must move that slot's bound read and leave
				// the other three on the set they came with - the panel reads
				// the World directly, so a shared write would show up here.
				world->setObjectImageSetResourceName("StationObjects");
				require(world->objectImageSetResourceName() == "StationObjects",
					"the Objects artwork selection did not reach the World the panel binds to");
				require(world->sectorImageSetResourceName() == "SectorAtlas",
					"the Sector selection moved with the Objects one");
				require(world->agentImageSetResourceName() == "AgentAtlas",
					"the Agent selection moved with the Objects one");
				require(world->furnitureImageSetResourceName() == "FurnitureAtlas",
					"the Furniture selection moved with the Objects one");
				// And the write is reversible through the same seam, so a
				// repoint is a selection rather than a one-way edit.
				world->setObjectImageSetResourceName("ObjectAtlas");
				require(panelSelections(*world) == std::vector<std::string>{
						"SectorAtlas", "ObjectAtlas", "AgentAtlas", "FurnitureAtlas" },
					"putting the Objects selection back did not restore the four defaults");
			} });
	}
}
