#pragma once

// The editor's live document (#971, spec #960).
//
// The editor's World is owned by a `game::WorldRuntime` - the same type the
// shipped game owns its World with - and a `game::WorldAuthoring` is wrapped
// over that runtime. The editor holds no World outside that arrangement, so
// the preview it shows is not a look-alike of the game's: it *is* the game's
// runtime path, with authoring powers added.
//
// Every drive call - tick, snapshot, events, pause/resume, time scale - goes
// through `runtime()`, which the document takes from its own authoring handle.
// Authoring goes through `authoring()`, and `world()` is the mutable view that
// handle grants, held as an aliasing `shared_ptr` so the many existing holders
// - the editing panels in particular - keep the runtime, and therefore the
// World, alive without any of them becoming a second owner (#972).
//
// The editor is a single-document application: one World, one runtime driving
// it, one authoring handle over that runtime. The free functions below are
// the only ways that document is opened, replaced or closed, which is what
// keeps the handle the UI holds and the runtime that drives it from drifting
// apart. `previewRuntime()` checks the World it is handed really is the open
// document's, so a stale handle fails loudly instead of driving a simulation
// nobody is looking at.
//
// The handle lives in pf-agent-editing rather than in the editor executable so
// the headless editor tier exercises the same arrangement the GUI drives -
// the claim that the preview is the game's runtime path is worth little if
// only the GUI can run it.

#include <cstdint>
#include <memory>
#include <string>

#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

namespace editor
{
	class EditorDocument
	{
	public:
		// Takes `world` into the runtime the document owns. Throws
		// std::invalid_argument when `world` is null, through WorldRuntime.
		explicit EditorDocument(std::unique_ptr<core::World> world);

		// The runtime this document's authoring handle wraps. Every drive call
		// the editor makes goes through here, so the preview ticks, pauses and
		// reports exactly what the game would.
		game::WorldRuntime& runtime() noexcept;

		game::WorldRuntime const& runtime() const noexcept;

		// The privileged mutator over the runtime. Every authoring call the
		// editor makes goes through this handle; it is the only mutable view of
		// the World the editor has (#972).
		game::WorldAuthoring& authoring() noexcept;

		// The World being shown, as the mutable view the authoring handle
		// grants over the runtime's World. Aliasing: the returned pointer
		// shares the runtime's lifetime and never the World's ownership.
		std::shared_ptr<core::World> world() noexcept;

		std::shared_ptr<const core::World> world() const noexcept;

	private:
		// Shared rather than unique so a holder of the document - the metrics
		// collector, for instance - can keep the runtime alive across the frame
		// in which the document is replaced. The World inside stays exclusively
		// the runtime's.
		std::shared_ptr<game::WorldRuntime> runtime_;
		std::unique_ptr<game::WorldAuthoring> authoring_;
	};

	// The document the editor currently has open, or null when none is.
	std::shared_ptr<EditorDocument> currentDocument() noexcept;

	// The runtime driving the preview of `world`. Throws std::logic_error when
	// no document is open, or when `world` is not the World that document's
	// runtime owns.
	game::WorldRuntime& previewRuntime(core::World const& world);

	// The same resolution that answers with null instead of throwing: null
	// when no document is open, or when `world` is not the World the open
	// document's runtime drives (#979).
	//
	// This exists for the render path. A render function that has already
	// called `ImGui::Begin` cannot carry a `std::logic_error` out through
	// its own `End`, so a World that stopped being the preview has to read
	// as "no runtime to show" rather than as an exception unwinding through
	// the window around it. Callers that are not rendering should keep using
	// the throwing form, which says which of the two conditions failed.
	game::WorldRuntime* tryPreviewRuntime(core::World const& world) noexcept;

	// Opens a new, empty document and returns the World the UI holds.
	std::shared_ptr<core::World> openNewWorld(std::string const& name,
		std::uint32_t cellsWide, std::uint32_t levelsHigh);

	// Takes a World the caller has built or loaded - a clipboard restore, a
	// `core::loadWorldDocument()` result - into a new document, and returns the
	// World the UI holds.
	//
	// This is the only adoption seam (#973). The World arrives as a
	// `unique_ptr` because that is the only shape the runtime can take without
	// lying about ownership: the loaders hand back exclusive ownership, so the
	// editor never holds a `shared_ptr<World>` it would have to convert. A
	// `shared_ptr` overload could only have been satisfied by fabricating a
	// `unique_ptr` over the same object and releasing the caller's reference,
	// which - when the caller was the sole owner, the normal case - destroyed
	// the World right there and left the runtime driving freed memory.
	std::shared_ptr<core::World> adoptWorld(std::unique_ptr<core::World> world);

	// Closes the document, releasing the runtime and the World it owns. Holders
	// of the World view keep it alive until they let go.
	void closeDocument() noexcept;

	// The handle the editor's UI carries (#972). The document owns the
	// runtime that owns the World and the authoring handle that changes it,
	// so holding the document holds the whole arrangement together; the
	// World only ever arrives through `authoring()` or the aliasing `world()`
	// view taken from it. Threading this rather than a `shared_ptr<World>` is
	// what stops the UI from holding a World that nothing authorises.
	using DocumentPtr = std::shared_ptr<EditorDocument>;
}
