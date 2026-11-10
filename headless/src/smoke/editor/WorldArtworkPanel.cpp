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
					"SectorAtlasNew", "ObjectAtlasNew", "AgentAtlasNew", "FurnitureAtlasNew" };
				require(shown == expected,
					"a fresh World's artwork panel does not show the new defaults: "
					+ shown[0] + ", " + shown[1] + ", " + shown[2] + ", " + shown[3]);
				// Every slot reads as the replacement family, so the panel is
				// not showing a mix that the loader would call out.
				for (auto const& name : shown)
					require(core::atlasFamilyOf(name) == core::AtlasFamily::Replacement,
						"a fresh World's artwork panel shows a set outside the new family: " + name);
				require(world->atlasFamilyDiagnostic().empty(),
					"a fresh World reports a mixed atlas family: " + world->atlasFamilyDiagnostic());
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
				require(runtime.sectorImageSetResourceName() == "SectorAtlasNew"
					&& runtime.objectImageSetResourceName() == "ObjectAtlasNew"
					&& runtime.agentImageSetResourceName() == "AgentAtlasNew"
					&& runtime.furnitureImageSetResourceName() == "FurnitureAtlasNew",
					"the runtime behind the artwork panel reads different sets than the panel shows");
			} });

		checks.push_back({ "world-artwork/repointedAtlasIsReportedBySlot", [](smoke::Context const&)
			{
				State state;
				CloseOnExit const closer;
				auto const world = editor::openNewWorld("Repointed artwork panel", 8, 2);
				require(world != nullptr, "the editor opened no World");
				// Repointing one of the four at the old family is the near-miss
				// the loader warns about, and the panel's bound read is the
				// same value the warning was built from.
				world->setObjectImageSetResourceName("ObjectAtlas");
				require(world->objectImageSetResourceName() == "ObjectAtlas",
					"the Objects artwork selection did not reach the World the panel binds to");
				auto const diagnostic = world->atlasFamilyDiagnostic();
				require(diagnostic.find("Objects=ObjectAtlas (old)") != std::string::npos,
					"repointing one atlas did not produce a straddle naming it: " + diagnostic);
				// The other three stay in the new family, which is the whole
				// point: the World is one three-character typo away from the
				// wrong sheet, and says so.
				require(core::atlasFamilyOf(world->sectorImageSetResourceName())
					== core::AtlasFamily::Replacement, "the Sector selection moved with the Objects one");
				require(core::atlasFamilyOf(world->agentImageSetResourceName())
					== core::AtlasFamily::Replacement, "the Agent selection moved with the Objects one");
				require(core::atlasFamilyOf(world->furnitureImageSetResourceName())
					== core::AtlasFamily::Replacement, "the Furniture selection moved with the Objects one");
				// Repointing the last old set back into the new family clears
				// the report, so the warning tracks the selection rather than
				// sticking once raised.
				world->setObjectImageSetResourceName("ObjectAtlasNew");
				require(world->atlasFamilyDiagnostic().empty(),
					"the straddle report survived the World being put back inside one family: "
					+ world->atlasFamilyDiagnostic());
			} });
	}
}
