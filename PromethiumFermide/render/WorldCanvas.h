#pragma once

#include "UISettings.h"
#include "WorldDrawList.h"
#include "WorldRenderStack.h"

namespace core { class World; }

namespace pf::application::render {

//
// The game's whole route to the shared renderer (#1002).
//
// Three calls, and nothing else. `create` hands the shared renderer the
// render infrastructure the host already owns and installs the game's
// MPP-backed font metrics; `render` records the World through the shared
// producer and submits the stream into the target the host has bound;
// `destroy` releases the canvas while the OpenGL context that created it is
// still current.
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

// Creates the shared renderer over the host's stack and installs the game's
// font metrics. Throws whatever `pf::render::createWorldRenderStack` throws
// when the render infrastructure or the Object atlas is unusable - a broken
// install is reported, not half-drawn.
void createWorldCanvas(WorldRenderStackHost const& host);

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

// Records and submits in one call. Does nothing without a live canvas.
void renderWorldCanvas(core::World const& world, float width, float height);

// The viewport the last record or render pass used, so a caller drawing an
// overlay in the same frame can place it against the World it sits on.
UISettings const& worldCanvasViewport();

} // namespace pf::application::render
