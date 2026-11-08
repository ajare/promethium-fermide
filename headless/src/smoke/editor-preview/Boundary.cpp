#include "Checks.h"

#include <memory>
#include <type_traits>

#include "EditorDocument.h"
#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;

	// The editor's document can only be built by handing the World over as
	// exclusive ownership. There is no constructor taking a `World&`, a
	// `World*` or a `shared_ptr<World>`, so the editor cannot keep a World of
	// its own and put a runtime beside it: "the editor obtains the runtime
	// from its WorldAuthoring rather than constructing a second World" is a
	// property of the type, not of how the call sites behave (#971).
	static_assert(std::is_constructible_v<editor::EditorDocument,
		std::unique_ptr<core::World>>);
	static_assert(!std::is_constructible_v<editor::EditorDocument, core::World&>);
	static_assert(!std::is_constructible_v<editor::EditorDocument, core::World*>);
	static_assert(!std::is_constructible_v<editor::EditorDocument,
		std::shared_ptr<core::World>>);

	// The document is one handle over one arrangement. Copying it would put
	// two authoring privileges over one World in circulation.
	static_assert(!std::is_copy_constructible_v<editor::EditorDocument>);
	static_assert(!std::is_copy_assignable_v<editor::EditorDocument>);

	// What the editor can reach through the document: a runtime to drive, an
	// authoring handle to change with, and the World they share. The runtime
	// accessor hands out the runtime itself and never a World, so a drive
	// call cannot quietly turn into a mutable World.
	static_assert(std::is_same_v<decltype(std::declval<editor::EditorDocument&>().runtime()),
		game::WorldRuntime&>);
	static_assert(std::is_same_v<decltype(std::declval<editor::EditorDocument const&>().runtime()),
		game::WorldRuntime const&>);
	static_assert(std::is_same_v<decltype(std::declval<editor::EditorDocument&>().authoring()),
		game::WorldAuthoring&>);
	static_assert(std::is_same_v<decltype(editor::previewRuntime(
		std::declval<core::World const&>())), game::WorldRuntime&>);
	static_assert(std::is_same_v<decltype(std::declval<editor::EditorDocument&>().world()),
		std::shared_ptr<core::World>>);
	static_assert(std::is_same_v<decltype(std::declval<editor::EditorDocument const&>().world()),
		std::shared_ptr<const core::World>>);
}

void registerEditorPreviewBoundary(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "thePreviewRuntimeIsTheAuthoringRuntime", [](smoke::Context const&)
		{
			struct CloseDocument { ~CloseDocument() { editor::closeDocument(); } } const closer;
			auto const world = editor::openNewWorld("Boundary", 4, 1);
			auto const document = editor::currentDocument();
			require(document != nullptr, "no document after opening a World");
			// The runtime the editor drives is the one its authoring handle
			// wraps, and the World inside it is the one the UI holds. One
			// World, one runtime, reached from the authoring side.
			require(&editor::previewRuntime(*world) == &document->runtime(),
				"the preview runtime is not the document's runtime");
			require(&editor::previewRuntime(*world).world()
					== &document->authoring().world(),
				"the driven runtime does not own the authored World");
		} });
}
