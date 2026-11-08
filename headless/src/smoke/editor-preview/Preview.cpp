#include "Checks.h"
#include "../game-runtime/Fixture.h"

#include <memory>
#include <utility>

#include "EditorDocument.h"
#include "core/Simulation.h"
#include "core/World.h"
#include "game/WorldRuntime.h"

namespace
{
	using editor_preview_smoke::canonical;
	using editor_preview_smoke::digest;
	using game_runtime_smoke::makeWalkingWorld;
	using smoke::require;

	struct CloseDocument
	{
		~CloseDocument() { editor::closeDocument(); }
	};
}

void registerEditorPreviewParity(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "thePreviewShowsWhatTheGameWouldForTheSameElapsedSeconds",
		[](smoke::Context const&)
		{
			CloseDocument const closer;
			// Two identical Worlds. One is driven the way the editor drives
			// its live preview - elapsed seconds through the document's
			// runtime - and one the way the shipped game drives its runtime,
			// tick by tick. Same World, same elapsed time, same result: the
			// preview is the game path rather than a likeness of it (#971).
			auto editorFixture = makeWalkingWorld("Parity");
			auto const previewWorld = editor::adoptWorld(std::move(editorFixture.world));
			auto gameFixture = makeWalkingWorld("Parity");
			game::WorldRuntime game(std::move(gameFixture.world));

			auto& preview = editor::previewRuntime(*previewWorld);
			for (int second = 0; second < 3; ++second)
			{
				preview.update(1.0f);
				game.advanceTicks(60);
				require(canonical(preview.getSimulationSnapshot())
						== canonical(game.getSimulationSnapshot()),
					"the preview and the game diverged after the same elapsed time");
				require(digest(preview.consumeSimulationEvents())
						== digest(game.consumeSimulationEvents()),
					"the preview and the game reported different events for the same elapsed time");
			}
			require(preview.getSimulationTick() == 180,
				"three seconds of preview did not run a hundred and eighty ticks");
		} });

	checks.push_back({ "aPausedPreviewRunsNoTicks", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Paused preview");
			auto const world = editor::adoptWorld(std::move(fixture.world));
			auto& preview = editor::previewRuntime(*world);

			// Let the Agent get moving first, so a preview that kept running
			// while paused would have somewhere obvious to have moved to.
			preview.update(0.5f);
			require(preview.getSimulationTick() == 30,
				"the preview did not run its first half second");
			preview.pauseSimulation();
			require(preview.isSimulationPaused(),
				"the preview did not report itself paused");
			auto const paused = canonical(preview.getSimulationSnapshot());
			preview.update(5.0f);
			require(preview.getSimulationTick() == 30,
				"a paused preview executed ticks while it was paused");
			require(canonical(preview.getSimulationSnapshot()) == paused,
				"the paused preview moved away from the state it paused in");

			require(preview.resumeSimulation(), "the preview refused to resume");
			require(!preview.isSimulationPaused(),
				"the preview still reported itself paused after resuming");
			preview.update(1.0f);
			require(preview.getSimulationTick() == 90,
				"the resumed preview did not pick the ticks back up");
		} });

	checks.push_back({ "thePreviewDrainsEachEventExactlyOnce", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Event drain");
			auto const world = editor::adoptWorld(std::move(fixture.world));
			auto& preview = editor::previewRuntime(*world);
			preview.update(1.0f);
			auto const drained = preview.consumeSimulationEvents();
			require(!drained.empty(),
				"a second of preview produced no events to drain");
			auto const again = preview.consumeSimulationEvents();
			require(again.empty(),
				"consuming events a second time returned events that had already been reported");
		} });

	checks.push_back({ "thePreviewSnapshotViewMatchesTheSnapshot", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Snapshot view");
			auto const world = editor::adoptWorld(std::move(fixture.world));
			auto& preview = editor::previewRuntime(*world);
			preview.update(0.5f);
			require(canonical(preview.getSimulationSnapshotView())
					== canonical(preview.getSimulationSnapshot()),
				"the preview's snapshot view and its snapshot disagree");
		} });

	checks.push_back({ "thePreviewHonoursTheTimeScale", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Time scale");
			auto const world = editor::adoptWorld(std::move(fixture.world));
			auto& preview = editor::previewRuntime(*world);
			preview.setSimulationTimeScale(2.0);
			require(preview.getSimulationTimeScale() == 2.0,
				"the preview's time scale did not read back");
			preview.update(0.5f);
			require(preview.getSimulationTick() == 60,
				"double time over half a second did not run sixty preview ticks");
			preview.setSimulationTimeScale(1.0);
			preview.update(0.5f);
			require(preview.getSimulationTick() == 90,
				"returning to normal time did not halve the ticks per second");
		} });

	checks.push_back({ "thePreviewReadsTheWorldTheAuthoringChanged", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Authoring visible");
			auto const world = editor::adoptWorld(std::move(fixture.world));
			auto const document = editor::currentDocument();
			require(document != nullptr, "no document after adopting a World");
			// An edit made through the authoring view is visible to the runtime
			// on the very next read: there is one World, and the preview is
			// looking at the thing the author changed.
			auto const before = document->runtime().world().getLayerCount();
			auto const added = document->authoring().addLayer();
			require(document->runtime().world().getLayerCount() == before + 1,
				"the runtime did not see the layer added through the authoring handle");
			require(added == before, "addLayer returned the wrong index");
			require(world->getLayerCount() == before + 1,
				"the World the UI holds is not the one the author edited");
		} });
}
