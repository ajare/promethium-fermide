#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "UISettings.h"
#include "WorldDrawList.h"
#include "WorldRenderStack.h"

namespace core { class World; }
namespace wp { class Logger; }

namespace pf::application::render {

//
// The game's whole route to the shared renderer (#1002).
//
// Three calls, and nothing else. `create` hands the shared renderer the
// render infrastructure the host already owns and installs the game's
// MPP-backed font metrics; `render` selects the World's four atlases,
// records the World through the shared producer and submits the stream
// into the target the host has bound; `destroy` releases the canvas while
// the OpenGL context that created it is still current.
//
// Selecting is one step of `render`, over one table (`worldAtlasSlots`)
// for the four World-wide atlases and one pass over the Layers for the
// Sector sheets they bind (#1026, #1094), because the World owns the
// artwork choice and the game owns nothing but handing all of it over
// every frame.
//
// The game keeps no renderer of its own behind these calls: no command
// stream class, no sector or object tilesets, no MPP consumer. The host's
// render system, resource managers and scene cross once, here, as the
// shared renderer's own host struct - the game's render code calls no MPP
// entry point and owns no MPP object.
//
// The camera is the game's, not the editor's: fitted to the World's content
// when no Player-controlled Agent exists, centred on the Player otherwise.
// What it produces is fed to the same producer the editor's World canvas
// uses, which is what makes the two canvases the same canvas.
//

// One viewport atlas slot, as the game hands it to the shared renderer:
// the slot's name for reporting, the World's selection for it, and the
// renderer entry that installs that selection and answers with its
// diagnostic - empty when the renderer accepted the set.
struct WorldAtlasSlot
{
	char const* slot;
	std::string const& (*selected)(core::World const&);
	std::string (*install)(std::string const&);
};

// The four viewport slots the game selects, in slot order.
//
// This table is the whole of the game's atlas selection: the render pass
// walks it every frame, so a slot missing here is a slot the game never
// selects and the renderer keeps drawing whatever it was constructed
// with. That is exactly what happened to Objects - the game selected three
// of the four World atlases and left the fourth at the renderer's default,
// so a World asking for the new Objects sheet drew the old one everywhere but
// in the editor (#1026). Walking one table is what makes the four slots
// one checkable list rather than four lines somebody has to count.
std::array<WorldAtlasSlot, 4> const& worldAtlasSlots();

// What one slot answered: the slot, the name that went through it, and the
// diagnostic that came back.
struct WorldAtlasSelection
{
	std::string slot;
	std::string name;
	std::string diagnostic;

	bool accepted() const { return diagnostic.empty(); }
};

// Pushes every slot's selection through the shared renderer, in slot
// order, and answers with each slot's result. A selection the renderer
// already has is a no-op inside it, so calling this every frame is how
// the game follows a live artwork change without owning the switch.
//
// A refused Objects atlas is answered with here rather than thrown over:
// the renderer leaves the slot empty - which draws magenta - and hands
// back the reason, which is what makes a World whose atlas cannot carry
// its object geometry visible instead of misaligned (#1022).
std::vector<WorldAtlasSelection> applyWorldAtlasSelections(core::World const& world);

// The entry a Layer's own Sector sheet is taken up through: it names the
// sheet, installs it beside the World's own, and answers with the install
// diagnostic - empty when the sheet is installed and resolves, and
// otherwise the reason it is not.
//
// The shipped game drives the live render stack's own entry
// (`installWorldLayerSectorImageSet`), which is the same entry the
// editor's Layers table drives (#1091). It is a parameter rather than a
// direct call for one reason: the render stack cannot exist without a
// live OpenGL context, while the install it runs is GPU-free by design
// (#1087). Taking the entry as an argument is what lets a CPU-only check
// drive the game's own sync over the real install path instead of
// re-implementing the sync beside it and proving nothing (#1094).
using LayerSectorSheetInstall = std::function<std::string(std::string const&)>;

// One Layer Sector sheet as the game's artwork sync took it up: the Layer
// that asked for it, the sheet it is bound to, and what the install
// answered.
struct WorldLayerSectorSheet
{
	std::uint32_t layer{};
	std::string name;
	std::string diagnostic;

	bool accepted() const { return diagnostic.empty(); }
};

// Takes up every distinct Sector sheet the Layers bind to, through
// `install`, and answers with one entry per sheet taken up.
//
// A Layer with no binding asks for nothing: it inherits the World's
// sector sheet, which the Sector slot already installed, and an empty
// binding name asks for nothing either. A Layer bound to the World's own
// set asks for nothing as well - that sheet is already in the registry
// under that name, and putting it through the alongside path a second
// time would replace the Sector slot's own entry behind the slot's back.
// Two Layers bound to one sheet take it up once, and the entry names the
// first of them.
//
// The install is cached per distinct name inside the renderer, so a
// frame that changed nothing costs the lookups below and no disk read.
std::vector<WorldLayerSectorSheet> applyWorldLayerSectorSheets(
	core::World const& world, LayerSectorSheetInstall const& install);

// Everything one frame's artwork sync answered.
struct WorldArtworkSync
{
	std::vector<WorldAtlasSelection> selections;
	std::vector<WorldLayerSectorSheet> layerSheets;
};

// The game's whole per-frame artwork sync, as one call: the four
// World-wide atlas selections, then every distinct Layer Sector binding.
//
// The order is the contract. A Layer binding is skipped when it names the
// World's own set, and the renderer makes the same skip against the
// Sector slot's current name; both only read correctly once the World's
// selection has gone through and that sheet is actually installed. Run
// the Layer half first and a Layer bound to the World's own set would be
// installed alongside a sheet that is not yet the drawn one (#1094).
WorldArtworkSync syncWorldArtwork(core::World const& world,
	LayerSectorSheetInstall const& install);

// The game's report for a set of slot answers: every diagnostic the
// renderer handed back, joined into the one message the game writes.
// Empty when every slot was accepted. Each diagnostic already names its
// own atlas and its own reason, so the report carries the refusal rather
// than restating it.
std::string worldAtlasReport(std::vector<WorldAtlasSelection> const& selections);

// Writes the refusals in `selections` to `report`, once per distinct
// report. The selection runs every frame, so the caller carries what was
// last written in `lastReported`: without it one bad atlas fills the log
// at frame rate. It clears when nothing is refused, so a World that
// recovers and then fails again is reported again rather than silenced
// by a stale memory. Returns true when this call wrote.
//
// This is the game's whole answer to a refused atlas. The game has no
// artwork panel to show a diagnostic in, so the log is what a player and
// a support pass get, and it has to carry the renderer's reason (#1026).
bool reportWorldAtlasSelections(wp::Logger& report,
	std::vector<WorldAtlasSelection> const& selections, std::string& lastReported);

// Creates the shared renderer over the host's stack and installs the game's
// font metrics. Throws whatever `pf::render::createWorldRenderStack` throws
// when the render infrastructure or the Object atlas is unusable - a broken
// install is reported, not half-drawn.
//
// `report` is where a refused atlas selection is written. The game has no
// artwork panel to show a diagnostic in, so it hands the canvas its own
// state logger; each distinct diagnostic is written once rather than once
// per frame (#1026).
void createWorldCanvas(WorldRenderStackHost const& host, wp::Logger* report = nullptr);

// Tears the canvas down. Safe to call when there is no canvas.
void destroyWorldCanvas();

// True while the canvas exists.
bool worldCanvasActive();

// Records the World's canvas commands for a width x height canvas with the
// game's camera. This is the seam the game's render pass is tested at: the
// same CPU command-stream seam the editor's World canvas is tested at,
// because it is the same producer. Leaves the viewport it recorded with
// readable through `worldCanvasViewport()`.
WorldDrawList drawWorld(core::World const& world, float width, float height);

// Records and submits in one call, syncing the World's artwork first -
// the four World-wide atlases and every distinct Layer Sector binding.
// Does nothing without a live canvas.
void renderWorldCanvas(core::World const& world, float width, float height);

// The viewport the last record or render pass used, so a caller drawing an
// overlay in the same frame can place it against the World it sits on.
UISettings const& worldCanvasViewport();

} // namespace pf::application::render
