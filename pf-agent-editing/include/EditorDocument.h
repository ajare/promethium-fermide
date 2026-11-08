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
// Authoring itself still reaches the raw World; #972 moves those calls onto
// `WorldAuthoring`. `world()` is the mutable view the authoring grants, held
// as an aliasing `shared_ptr` so the many existing holders keep the runtime -
// and therefore the World - alive without any of them becoming a second owner.
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

		// The privileged mutator over the runtime. Authoring reaches the
		// World through it; #972 moves the editor's authoring calls onto it.
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

	// Opens a new, empty document and returns the World the UI holds.
	std::shared_ptr<core::World> openNewWorld(std::string const& name,
		std::uint32_t cellsWide, std::uint32_t levelsHigh);

	// Takes a World the caller has built - a clipboard restore, say - into a
	// new document, and returns the World the UI holds.
	std::shared_ptr<core::World> adoptWorld(std::unique_ptr<core::World> world);

	// Same, from the `shared_ptr` the document loaders hand back. The loader's
	// reference must be the only one: the runtime owns its World exclusively,
	// so a World something else still holds is refused rather than quietly
	// shared behind the runtime's back.
	std::shared_ptr<core::World> adoptWorld(
		std::shared_ptr<core::World>& loaded);

	// Closes the document, releasing the runtime and the World it owns. Holders
	// of the World view keep it alive until they let go.
	void closeDocument() noexcept;
}
