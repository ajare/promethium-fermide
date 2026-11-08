#include "Checks.h"
#include "../game-runtime/Fixture.h"

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "EditorDocument.h"
#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

namespace
{
	using game_runtime_smoke::makeWalkingWorld;
	using smoke::require;

	struct CloseDocument
	{
		~CloseDocument() { editor::closeDocument(); }
	};

	// What the editor's UI carries is the document, not a World (#972). The
	// document owns the runtime that owns the World and the authoring handle
	// that changes it, so the mutable World is only ever reachable through
	// `WorldAuthoring`: `WorldRuntime` hands out `World const&` and nothing
	// else, which is what makes "no editor code mutates without going
	// through the authoring handle" a property of the types rather than a
	// habit of the call sites.
	static_assert(std::is_same_v<editor::DocumentPtr,
		std::shared_ptr<editor::EditorDocument>>);
	static_assert(std::is_same_v<decltype(std::declval<editor::EditorDocument&>().authoring()),
		game::WorldAuthoring&>);
	static_assert(std::is_same_v<decltype(std::declval<game::WorldRuntime&>().world()),
		core::World const&>);
	static_assert(std::is_same_v<decltype(std::declval<game::WorldRuntime const&>().world()),
		core::World const&>);

	// The authoring privilege is one handle over one World. Copying it would
	// put two of them in circulation over the same document.
	static_assert(!std::is_copy_constructible_v<game::WorldAuthoring>);
	static_assert(!std::is_copy_assignable_v<game::WorldAuthoring>);

	// A preflight guard is declared on a const handle, so asking whether an
	// edit is legal is answered from the read view and never spends the
	// mutable one. These are the shapes the editor's `can...` calls ride.
	static_assert(std::is_invocable_r_v<bool,
		decltype(&game::WorldAuthoring::canAddBackground),
		game::WorldAuthoring const&, uint32_t, uint32_t, uint32_t, uint32_t,
		uint32_t, std::string*>);
	static_assert(std::is_invocable_r_v<bool,
		decltype(&game::WorldAuthoring::canAddAccessPanel),
		game::WorldAuthoring const&, uint32_t, uint32_t, uint32_t,
		core::AccessPanelGeometry, std::string*>);
}

void registerEditorPreviewAuthoringRoute(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "theEditorAuthorsIntoTheWorldItPreviews", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Authoring route");
			auto const world = editor::adoptWorld(std::move(fixture.world));
			auto const document = editor::currentDocument();
			require(document != nullptr, "no document after adopting a World");

			// One World, reached three ways: the view the UI holds, the
			// mutable view the authoring handle grants, and the const view
			// the runtime hands to rendering.
			require(world.get() == &document->authoring().world(),
				"the World the UI holds is not the World the authoring changes");
			require(&document->runtime().world() == &document->authoring().world(),
				"the previewed World is not the authored World");

			auto const sectorsBefore = document->runtime().world().getNumSectors();
			// The edit protocol the World enforces: pause at the edit boundary,
			// edit, rebuild. The handle forwards all three and adds nothing.
			document->runtime().pauseSimulation();
			auto const room = document->authoring().addRoom("Authored", 1, 0, 0, 3, 1);
			document->authoring().finishBuild();
			require(document->runtime().world().getNumSectors() == sectorsBefore + 1,
				"the preview did not gain the Sector authored through the handle");
			require(document->runtime().world().getSector(room) != nullptr,
				"the authored Sector is not readable through the preview");

			// A rename is an authoring act too, and reaches the same World.
			document->authoring().setLayerName(1, "Upper");
			require(document->runtime().world().getLayerName(1) == "Upper",
				"the layer renamed through the handle reads back under its old name");
		} });

	checks.push_back({ "theMutableWorldThePanelsTakeIsTheAuthoringWorld",
		[](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Panel view");
			auto const world = editor::adoptWorld(std::move(fixture.world));
			auto const document = editor::currentDocument();
			require(document != nullptr, "no document after adopting a World");
			// The editing panels take a mutable World and are unchanged by
			// this ticket; what changed is where that World comes from. It is
			// the object the authoring handle grants over the preview's
			// runtime, so a panel editing through it is editing the thing
			// the preview is running - not a copy, and not a second World.
			require(world.get() == &document->authoring().world(),
				"the World handed to the panels is not the authoring's World");
			require(world.use_count() > 1,
				"the World view does not share the runtime's control block");

			// A change made through the handle shows up in the object the
			// panels hold, on the next read and with no refresh step: it is
			// the same World, not a copy that has to be re-sent.
			auto const sectorsBefore = world->getNumSectors();
			document->runtime().pauseSimulation();
			document->authoring().addRoom("Panel authored", 1, 0, 0, 2, 1);
			document->authoring().finishBuild();
			require(world->getNumSectors() == sectorsBefore + 1,
				"the World the panels hold did not gain what the handle authored");
			require(&document->authoring().world() == world.get(),
				"the handle and the panel view drifted apart after the edit");
		} });

	checks.push_back({ "preflightAnswersFromAConstHandle", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Preflight");
			auto const world = editor::adoptWorld(std::move(fixture.world));
			auto const document = editor::currentDocument();
			require(document != nullptr, "no document after adopting a World");
			// The const handle answers the guard, and answers it the same way
			// the mutable handle does: the check is a read of the document.
			game::WorldAuthoring const& readonly = document->authoring();
			std::string diagnostic;
			bool const allowed = readonly.canAddBackground(1, 0, 0, 3, 1, &diagnostic);
			std::string const mutableAnswer = document->authoring()
				.canAddBackground(1, 0, 0, 3, 1, &diagnostic) ? "yes" : "no";
			require((allowed ? "yes" : "no") == mutableAnswer,
				"the const and mutable handles disagree about the same preflight");
			require(allowed,
				"an empty Layer refused a Background: " + diagnostic);
		} });

	checks.push_back({ "theAuthoringHandleWrapsThePreviewRuntime", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Wrapped");
			auto const world = editor::adoptWorld(std::move(fixture.world));
			auto const document = editor::currentDocument();
			require(document != nullptr, "no document after adopting a World");
			// The handle the editor authors with wraps the very runtime the
			// preview drives, which is the arrangement #971 established and
			// what #972 now routes every authoring call through.
			require(&document->authoring().runtime()
					== &editor::previewRuntime(*world),
				"the authoring handle does not wrap the preview runtime");
			require(&document->authoring().document()
					== &document->runtime().world(),
				"the authoring read view is not the previewed document");
		} });
}
