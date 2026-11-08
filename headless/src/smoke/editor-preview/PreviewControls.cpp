#include "Checks.h"
#include "../game-runtime/Fixture.h"

#include <memory>
#include <stdexcept>
#include <string>

#include "EditorDocument.h"
#include "PreviewControls.h"
#include "core/World.h"
#include "game/WorldRuntime.h"

namespace
{
	using game_runtime_smoke::makeWalkingWorld;
	using smoke::require;

	// Each check opens the document it needs and closes it again, so the
	// single-document invariant is left as it was found.
	struct CloseDocument
	{
		~CloseDocument() { editor::closeDocument(); }
	};

	// The action behind the editor's global S shortcut with ImGui taken out
	// of it (#979): guard on a document whose preview can actually be
	// driven, then ask for the opposite of the state the indicator shows.
	// The editor's own `gUISettings.worldPaused` is passed in as
	// `shownPaused` because the setting lives in the GUI; the contract being
	// pinned is what the shortcut does to the preview, not what it paints.
	struct ShortcutOutcome
	{
		bool acted{ false };
		bool paused{ false };
		std::string diagnostic;
	};

	ShortcutOutcome pressPauseShortcut(
		editor::DocumentPtr const& document, bool shownPaused)
	{
		if (!editor::previewIsControllable(document))
			return ShortcutOutcome{ false, shownPaused, "" };
		auto const result = editor::setPreviewPaused(document, !shownPaused);
		if (!result.handled) return ShortcutOutcome{ false, shownPaused, "" };
		return ShortcutOutcome{ true, result.paused, result.diagnostic };
	}

	// The same for R: the shortcut asks for a reset and only touches its
	// own state when something answered.
	ShortcutOutcome pressResetShortcut(editor::DocumentPtr const& document)
	{
		if (!editor::previewIsControllable(document))
			return ShortcutOutcome{ false, false, "" };
		auto const result = editor::resetPreview(document);
		if (!result.handled) return ShortcutOutcome{ false, false, "" };
		return ShortcutOutcome{ true, result.paused, result.diagnostic };
	}
}

void registerEditorPreviewSimulationControls(std::vector<smoke::Check>& checks)
{
	// The case the shortcut reaches but no document does: nothing is open,
	// S is pressed, and the request has to be a clean no-op rather than a
	// null dereference or a throw out of the frame.
	checks.push_back({ "pauseShortcutWithNoDocumentOpenIsACleanNoOp",
		[](smoke::Context const&)
		{
			editor::closeDocument();
			bool threw = false;
			ShortcutOutcome outcome{ true, true };
			try
			{
				outcome = pressPauseShortcut(nullptr, false);
			}
			catch (std::exception const&)
			{
				threw = true;
			}
			require(!threw,
				"the pause shortcut threw with no document open");
			require(!outcome.acted,
				"the pause shortcut acted with no document open");
			require(!outcome.paused,
				"the pause shortcut changed the shown paused state with no document open");
			require(!editor::previewIsControllable(nullptr),
				"a null document was reported as controllable");
		} });

	checks.push_back({ "resetShortcutWithNoDocumentOpenIsACleanNoOp",
		[](smoke::Context const&)
		{
			editor::closeDocument();
			bool threw = false;
			ShortcutOutcome outcome{ true, true };
			try
			{
				outcome = pressResetShortcut(nullptr);
			}
			catch (std::exception const&)
			{
				threw = true;
			}
			require(!threw,
				"the reset shortcut threw with no document open");
			require(!outcome.acted,
				"the reset shortcut acted with no document open");
		} });

	// The control seam itself, refused: nothing was paused, nothing was
	// started, and no diagnostic was invented for a refusal that was never
	// tested against a World.
	checks.push_back({ "controllingNoDocumentReportsNothingWasHandled",
		[](smoke::Context const&)
		{
			editor::closeDocument();
			auto const pause = editor::setPreviewPaused(nullptr, true);
			require(!pause.handled,
				"pausing was reported handled with no document open");
			require(!pause.paused,
				"pausing reported a paused state that was never applied");
			require(pause.diagnostic.empty(),
				"pausing with no document open produced a diagnostic");

			auto const reset = editor::resetPreview(nullptr);
			require(!reset.handled,
				"a reset was reported handled with no document open");
			require(reset.diagnostic.empty(),
				"a reset with no document open produced a diagnostic");
		} });

	// A handle to a document the editor has already replaced is refused the
	// same way as no handle at all. This is the case a bare `world != nullptr`
	// check lets through: the pointer is live, but the thing it names is no
	// longer what the editor is showing.
	checks.push_back({ "aReplacedDocumentHandleIsRefusedRatherThanDriven",
		[](smoke::Context const&)
		{
			CloseDocument const closer;
			auto const first = editor::openNewWorld("First", 8, 2);
			auto const stale = editor::currentDocument();
			require(stale != nullptr, "no handle after opening the first World");
			require(editor::previewIsControllable(stale),
				"a live document was reported uncontrollable");

			editor::openNewWorld("Second", 8, 2);
			auto const live = editor::currentDocument();
			require(live != stale,
				"opening a second World did not replace the document");
			require(!editor::previewIsControllable(stale),
				"a replaced document handle was still reported controllable");

			auto const pause = editor::setPreviewPaused(stale, true);
			require(!pause.handled,
				"a replaced document was paused through its stale handle");
			require(!live->runtime().isSimulationPaused(),
				"pausing a stale handle paused the live document");

			auto const reset = editor::resetPreview(stale);
			require(!reset.handled,
				"a replaced document was reset through its stale handle");
		} });

	// The accepting path, so the refusals above mean something: with the
	// open document in hand the same calls drive the same runtime the
	// preview ticks. The fixture is a built, resumable World - a bare
	// `openNewWorld` has never had its topology validated, so a resume on
	// it is refused for the World's own reasons rather than the seam's.
	checks.push_back({ "controllingTheOpenDocumentDrivesItsRuntime",
		[](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Controlled");
			auto const world = editor::adoptWorld(std::move(fixture.world));
			auto const document = editor::currentDocument();
			require(world != nullptr && document != nullptr,
				"no document after adopting a World");

			auto const outcome = pressPauseShortcut(document, false);
			require(outcome.acted,
				"the pause shortcut did not act on the open document");
			require(outcome.paused,
				"the pause shortcut did not report the preview paused");
			require(document->runtime().isSimulationPaused(),
				"the runtime the preview drives is not paused");

			auto const resumed = pressPauseShortcut(document, true);
			require(resumed.acted,
				"the pause shortcut did not act on the open document when resuming");
			require(resumed.diagnostic.empty(),
				"resuming a built World was refused: " + resumed.diagnostic);
			require(!resumed.paused,
				"resuming a built World still reported the preview paused");
			require(!document->runtime().isSimulationPaused(),
				"the runtime the preview drives is still paused after resuming");
		} });

	// A reset reaches the runtime it is aimed at: the tick counter goes
	// back to zero through the seam. The paused state is the World's own
	// answer - `resetSimulation` keeps whatever it was showing - so the
	// check requires the reported state to be the runtime's, not a
	// particular value.
	checks.push_back({ "resettingTheOpenDocumentDrivesItsRuntime",
		[](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Reset me");
			auto const world = editor::adoptWorld(std::move(fixture.world));
			auto const document = editor::currentDocument();
			require(world != nullptr && document != nullptr,
				"no document after adopting a World");
			document->runtime().update(1.0f);
			require(document->runtime().getSimulationTick() > 0,
				"the fixture did not run any ticks before the reset");

			auto const outcome = pressResetShortcut(document);
			require(outcome.acted,
				"the reset shortcut did not act on the open document");
			require(document->runtime().getSimulationTick() == 0,
				"the reset did not reach the runtime the preview drives");
			require(outcome.paused == document->runtime().isSimulationPaused(),
				"the reset reported a paused state other than the runtime's");
		} });

	// The non-throwing resolve the render path uses answers the two cases
	// `previewRuntime` throws on with a null pointer instead, which is how
	// a render function keeps its window open when the World it was handed
	// stopped being the preview (#979).
	checks.push_back({ "tryPreviewRuntimeAnswersNullWherePreviewRuntimeThrows",
		[](smoke::Context const&)
		{
			editor::closeDocument();
			core::World loose("Loose", 4, 1);
			require(editor::tryPreviewRuntime(loose) == nullptr,
				"tryPreviewRuntime named a runtime with no document open");
			bool threw = false;
			try
			{
				(void)editor::previewRuntime(loose);
			}
			catch (std::logic_error const&)
			{
				threw = true;
			}
			require(threw,
				"previewRuntime stopped refusing a World with no document open");

			CloseDocument const closer;
			auto const world = editor::openNewWorld("Driven", 8, 2);
			require(editor::tryPreviewRuntime(*world)
					== &editor::currentDocument()->runtime(),
				"tryPreviewRuntime did not resolve the open document's runtime");
			core::World foreign("Foreign", 4, 1);
			require(editor::tryPreviewRuntime(foreign) == nullptr,
				"tryPreviewRuntime named a runtime for a World it does not drive");
		} });
}
