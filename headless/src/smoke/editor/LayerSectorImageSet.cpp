#include "Checks.h"
#include "State.h"

#include "DocumentEdit.h"
#include "EditorDocument.h"
#include "LayerSectorImageSet.h"
#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

#include <memory>
#include <string>
#include <vector>

// The Layers table binds a Layer's Sector Image set through the authoring
// seam, and every word the table shows about that binding is a read of the
// World the panel is bound to (#1091). This drives that seam the way the
// row drives it - snapshot, write, commit - rather than the ImGui widgets,
// for the same reason the Artwork panel checks do: the widgets have no state
// of their own to be wrong, and everything the row can get wrong is a fact
// about the World or about the two sentences built from it.
//
// What is pinned here, in the order the author meets it:
//
//   - a fresh Layer reads as inherit, and the inherit wording names the
//     World's own set rather than leaving the author to guess it;
//   - a pick reaches the World the panel binds to, and the runtime behind it
//     - which is the thing the viewport draws - reads the same name back;
//   - the pick is one undo step, and redo puts it back;
//   - the pick moves that Layer's binding and nothing else: not the other
//     Layers', not the four World-wide atlas selections;
//   - clearing the pick restores inherit through the same seam, and the
//     Artwork panel's override line goes quiet again.
//
// What is *not* here is the install. Whether a bound sheet actually resolves
// is the renderer's answer, driven through the real install path by the
// Render tier's own checks (#1087, #1089, #1090); the Layers table shows
// whatever that answer was, and this file has no opinion about it.

namespace
{
	using smoke::require;

	// Closes whatever document the check opened, so a later check in the
	// same process never reads a World this one left behind.
	struct CloseOnExit
	{
		~CloseOnExit() { editor::closeDocument(); }
	};

	// The four World-wide atlas selections, in the order the Artwork panel
	// shows them. A Layer binding is a fifth, separate write; a Layer pick
	// that moved any of these would show up here as a set the author left
	// alone coming back changed.
	std::vector<std::string> worldAtlasSelections(core::World const& world)
	{
		return { world.sectorImageSetResourceName(), world.objectImageSetResourceName(),
			world.agentImageSetResourceName(), world.furnitureImageSetResourceName() };
	}

	std::vector<std::string> expectedWorldAtlasSelections()
	{
		return { "SectorAtlas", "ObjectAtlas", "AgentAtlas", "FurnitureAtlas" };
	}

	// Every Layer's binding as a string, with the inherit state spelled out
	// so a failed comparison says which Layer drifted and in which direction.
	std::vector<std::string> layerBindings(core::World const& world)
	{
		std::vector<std::string> bindings;
		for (std::uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
		{
			auto const& binding = world.layerSectorImageSet(layer);
			bindings.push_back(binding ? *binding : std::string("<inherit>"));
		}
		return bindings;
	}

	std::string bindingsText(std::vector<std::string> const& bindings)
	{
		std::string text;
		for (auto const& binding : bindings)
		{
			if (!text.empty()) text += ", ";
			text += binding;
		}
		return text;
	}

	// The write a Layers table row performs on a pick: one snapshot taken
	// before, one commit after, through the document's own history.
	void pick(editor::EditorDocument& document, std::uint32_t layer,
		std::optional<std::string> imageSet)
	{
		auto& authoring = document.authoring();
		auto const before = authoring.captureSnapshot();
		authoring.setLayerSectorImageSet(layer, std::move(imageSet));
		authoring.commitEdit(before);
	}
}

namespace editor_smoke
{
	void registerLayerSectorImageSet(std::vector<smoke::Check>& checks)
	{
		// A fresh Layer has no binding, and the words the row shows for that
		// state name the World's own set. Both halves matter: an optional
		// that reads empty is worth little if the panel cannot say what the
		// empty stands for, and wording that names the wrong set is worse
		// than no wording at all.
		checks.push_back({ "layer-image-set/aFreshLayerReadsAsInheritAndIsSaidTo",
			[](smoke::Context const&)
			{
				State state;
				CloseOnExit const closer;
				auto const world = editor::openNewWorld("Fresh Layer binding", 8, 2);
				require(world != nullptr, "the editor opened no World");
				auto const document = editor::currentDocument();
				require(document != nullptr, "the editor opened no document");
				require(world->getLayerCount() >= 2,
					"a fresh World came with fewer than two Layers");
				for (std::uint32_t layer = 0; layer < world->getLayerCount(); ++layer)
				{
					require(!world->layerSectorImageSet(layer).has_value(),
						"a fresh Layer carries a Sector Image set binding: "
							+ world->getLayerName(layer));
					// The runtime behind the panel - the thing the viewport
					// draws - reads the same absence, so the row and the
					// canvas cannot disagree about who is inheriting.
					require(!document->runtime()
							.layerSectorImageSet(layer).has_value(),
						"the runtime behind the Layers table reads a binding the "
						"World does not have: " + world->getLayerName(layer));
				}
				require(editor::layerSectorInheritLabel(world->sectorImageSetResourceName())
					== "Inherit (SectorAtlas)",
					"the inherit wording does not name the World's own Sector set: "
					+ editor::layerSectorInheritLabel(world->sectorImageSetResourceName()));
				// With nothing overriding, the Artwork panel has nothing to
				// say under the Sector combo.
				require(editor::layerSectorOverrideSummary(*world).empty(),
					"the Artwork panel reports overrides on a World that has none: "
					+ editor::layerSectorOverrideSummary(*world));
			} });

		// The pick reaches the World the panel binds to, and the runtime
		// behind it reads the same name. That is the whole of the row's
		// promise: what the combo shows is what the World holds.
		checks.push_back({ "layer-image-set/aPickReachesTheWorldThePanelBindsTo",
			[](smoke::Context const&)
			{
				State state;
				CloseOnExit const closer;
				auto const world = editor::openNewWorld("Layer pick", 8, 2);
				require(world != nullptr, "the editor opened no World");
				auto const document = editor::currentDocument();
				require(document != nullptr, "the editor opened no document");
				pick(*document, 1, std::string("SectorAtlasLayer1"));
				require(world->layerSectorImageSet(1)
						== std::optional<std::string>("SectorAtlasLayer1"),
					"the Layer pick did not reach the World the panel binds to");
				require(document->runtime().layerSectorImageSet(1)
						== std::optional<std::string>("SectorAtlasLayer1"),
					"the runtime behind the Layers table reads a different Sector "
					"set than the World holds");
				// And the row's own words follow: the Layer is no longer
				// inheriting, so the Artwork panel names it.
				require(editor::layerSectorOverrideSummary(*world)
						== "Layers with their own Sector sheet: "
							+ world->getLayerName(1)
							+ ". The set above is what every other Layer draws.",
					"the Artwork panel's override line does not name the Layer that "
					"just took a sheet of its own: "
					+ editor::layerSectorOverrideSummary(*world));
			} });

		// One pick is one undo step, and redo puts it back. The step is the
		// document's own, so the same Undo the author presses after naming a
		// Layer takes the binding back and nothing else with it.
		checks.push_back({ "layer-image-set/onePickIsOneUndoStepAndRedoes",
			[](smoke::Context const&)
			{
				State state;
				CloseOnExit const closer;
				auto const world = editor::openNewWorld("Layer pick undo", 8, 2);
				require(world != nullptr, "the editor opened no World");
				auto const document = editor::currentDocument();
				require(document != nullptr, "the editor opened no document");
				// A new World is unsaved work until it is marked otherwise, so
				// the baseline for "this pick dirtied it" is a saved document
				// rather than a freshly opened one.
				document->authoring().markSaved();
				require(!document->authoring().isModified(),
					"a marked-saved document arrived modified before any Layer pick");
				pick(*document, 1, std::string("SectorAtlasLayer1"));
				require(document->authoring().isModified(),
					"a Layer Sector pick did not dirty the document");
				require(document->authoring().undoDepth() == 1,
					"a Layer Sector pick took "
						+ std::to_string(document->authoring().undoDepth())
						+ " undo steps instead of one");
				// Undoing the pick returns the Layer to inherit.
				require(document->authoring().undo(),
					"undoing a Layer Sector pick reported failure");
				require(!world->layerSectorImageSet(1).has_value(),
					"undoing a Layer Sector pick left the Layer bound to: "
					+ world->layerSectorImageSet(1).value_or("<still bound>"));
				// Redoing it puts the same pick back.
				require(document->authoring().redo(),
					"redoing a Layer Sector pick reported failure");
				require(world->layerSectorImageSet(1)
						== std::optional<std::string>("SectorAtlasLayer1"),
					"redoing a Layer Sector pick did not restore the picked set");
			} });

		// A Layer binding is one Layer's own fact. Two Layers can hold
		// different sets at once, and neither write reaches the other or the
		// four World-wide atlas selections beside them.
		checks.push_back({ "layer-image-set/aPickMovesNoOtherLayerAndNoWorldAtlas",
			[](smoke::Context const&)
			{
				State state;
				CloseOnExit const closer;
				auto const world = editor::openNewWorld("Layer pick isolation", 8, 2);
				require(world != nullptr, "the editor opened no World");
				auto const document = editor::currentDocument();
				require(document != nullptr, "the editor opened no document");
				require(worldAtlasSelections(*world) == expectedWorldAtlasSelections(),
					"a fresh World does not start on the canonical four sets");
				pick(*document, 1, std::string("SectorAtlasLayer1"));
				require(layerBindings(*world) == std::vector<std::string>{
						"<inherit>", "SectorAtlasLayer1" },
					"the first Layer pick moved a Layer it was not asked about: "
					+ bindingsText(layerBindings(*world)));
				// A second Layer, a different set, and the first stays put.
				pick(*document, 0, std::string("SectorAtlasLayer2"));
				require(layerBindings(*world) == std::vector<std::string>{
						"SectorAtlasLayer2", "SectorAtlasLayer1" },
					"the second Layer pick moved the first Layer's binding: "
					+ bindingsText(layerBindings(*world)));
				// And the four World-wide selections are untouched by both.
				require(worldAtlasSelections(*world) == expectedWorldAtlasSelections(),
					"a Layer Sector pick moved one of the four World-wide atlas "
					"selections: " + bindingsText(worldAtlasSelections(*world)));
				// Both Layers now show up in the Artwork panel's line, in
				// Layer order.
				require(editor::layerSectorOverrideSummary(*world)
						== "Layers with their own Sector sheet: "
							+ world->getLayerName(0) + ", " + world->getLayerName(1)
							+ ". The set above is what every other Layer draws.",
					"the Artwork panel's override line does not name both overriding "
					"Layers in order: " + editor::layerSectorOverrideSummary(*world));
			} });

		// Clearing is the same seam with nothing in it, and it restores
		// inherit rather than leaving an empty name that resolves to nothing.
		// The distinction matters: an empty binding is a name that matches no
		// sheet, and the row would keep showing a set the World does not have.
		checks.push_back({ "layer-image-set/clearingRestoresInherit",
			[](smoke::Context const&)
			{
				State state;
				CloseOnExit const closer;
				auto const world = editor::openNewWorld("Layer clear", 8, 2);
				require(world != nullptr, "the editor opened no World");
				auto const document = editor::currentDocument();
				require(document != nullptr, "the editor opened no document");
				pick(*document, 1, std::string("SectorAtlasLayer1"));
				require(world->layerSectorImageSet(1).has_value(),
					"the pick before the clear did not land");
				pick(*document, 1, std::nullopt);
				require(!world->layerSectorImageSet(1).has_value(),
					"clearing a Layer Sector binding left a value behind: "
					+ world->layerSectorImageSet(1).value_or("<still set>"));
				require(!document->runtime().layerSectorImageSet(1).has_value(),
					"the runtime behind the Layers table still reads a cleared binding");
				// Cleared means inherit again, which means the Artwork panel
				// has nothing to report.
				require(editor::layerSectorOverrideSummary(*world).empty(),
					"the Artwork panel still reports overrides after every Layer was "
					"cleared: " + editor::layerSectorOverrideSummary(*world));
				// And the clear is itself one undo step, so the pick it
				// removed can be put back.
				require(document->authoring().undoDepth() == 2,
					"a Layer Sector clear was not its own undo step; the stack holds "
					+ std::to_string(document->authoring().undoDepth()) + " steps");
				require(document->authoring().undo(),
					"undoing the clear reported failure");
				require(world->layerSectorImageSet(1)
						== std::optional<std::string>("SectorAtlasLayer1"),
					"undoing the clear did not put the original pick back");
			} });
	}
}
