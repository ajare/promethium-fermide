#pragma once

#include <memory>
#include <string>

#include "DrawTypes.h"
#include "ObjectTileset.h"
#include "SectorTileset.h"
#include "TileArtwork.h"

namespace wp::application::resourcesystem
{
	class Resource;
}

// The shared renderer's atlas install path (#1014, driven against the shipped
// manifest by #1030).
//
// This is the code `WorldRenderStack` runs to turn the application's registered
// `ImageSet` resources into the four viewport tilesets. It is kept out of the
// render stack class for one reason: the stack cannot exist without a live OpenGL
// context, and nothing in the install path needs one. The pairing is pure CPU -
// a decoded `ImageSet` on one side, an authored tileset YAML file on the other -
// so taking it out of the class is what lets a headless check drive the very
// functions the application runs instead of re-implementing them beside it.
//
// That distinction is the whole point. Every install assertion written before
// #1030 called `installAtlasTiles()` by hand, which proved the helper while the
// real caller went untested; four defects (#1025, #1026, #1028, #1029) stayed
// green behind it. `WorldRenderStack` now holds a `ViewportAtlasSlots` and calls
// `installAtlasDefinitions()` exactly as a check does, so there is no caller
// left that can drift away from the path under test.
//
// The two authorities this path keeps apart, in the words of `TileArtwork.h`:
// the pixel region is the `ImageSet`'s, and the tintable flag, render offset
// and drawn size are the authored tileset's. Neither is derived from the other.

namespace pf::render
{
	// A viewport atlas resource as the application's resource manager hands it
	// out. Named here so the install path's signature says what it installs
	// without pulling the Willpower resource headers into every includer.
	using AtlasResource = std::shared_ptr<wp::application::resourcesystem::Resource>;

	// Which authored tileset format sits behind an atlas. Both produce the same
	// render semantics; only the file shape differs.
	enum class AtlasAuthoredFormat { ObjectTiles, SectorTiles };

	// One viewport atlas as the install path read it: the pixel regions its
	// `ImageSet` declares, each paired with the render semantics the authored
	// tileset declares under the same tile name.
	struct InstalledAtlas
	{
		int width{}, height{};
		TileArtworkMap tiles;
		// Whether the authored tileset declared its own drawn size and render
		// offset, which is what makes it checkable against the Objects
		// contract (#1022).
		bool declaresRenderSemantics{ false };
		// The artwork family the authored set belongs to, carried through for
		// the Sector slot, which gates the surfaces its own-tile types wear on
		// the family the artwork was authored in (#1027). The object-shaped
		// slots read nothing from it: an object tileset's version says nothing
		// about which surface a Sector type wears.
		SectorTilesetFamily family{ SectorTilesetFamily::Old };
	};

	// The pairing half of the install path, and the only place a drawn region
	// is chosen.
	//
	// Pixel regions come from the atlas's `ImageSet` resource - the runtime
	// pixel-region contract ADR 0010 names, and the only place a drawn region
	// is taken from. The render semantics that go with each tile come from the
	// authored tileset that the atlas-set registry names for this atlas, under
	// the `tileset` option. Nothing here derives a tint from a tile's name or
	// a drawn size from a domain object's geometry.
	//
	// The pairing itself is done by `installAtlasTiles()`: it walks the
	// `ImageSet`'s regions and takes only the render semantics from the
	// tileset. A region that escapes the atlas image, and a tile the `ImageSet`
	// declares that the tileset never authored, are both refused there rather
	// than installed as artwork that samples outside the sheet (#1025).
	//
	// Throws `std::runtime_error` when `set` is not a loadable `ImageSet`, when
	// the atlas-set registry does not name an authored tileset of `format` for
	// it, or when the two cannot be paired.
	InstalledAtlas installAtlasArtwork(std::string const& setName,
		AtlasResource const& set, AtlasAuthoredFormat format);

	// The four viewport atlas slots as the shared renderer holds them: which
	// resource each slot is named for, what is loaded behind it, and the
	// diagnostic its last install produced.
	//
	// A slot with an empty diagnostic installed cleanly; a non-empty one holds
	// the reason it did not, which the host reports and the canvas draws as
	// magenta.
	struct ViewportAtlasSlots
	{
		std::string sectorName{ "SectorAtlas" };
		std::string objectName{ "ObjectAtlas" };
		std::string agentName{ "AgentAtlas" };
		std::string furnitureName{ "FurnitureAtlas" };
		AtlasResource sector;
		AtlasResource objects;
		AtlasResource agent;
		AtlasResource furniture;
		std::string sectorDiagnostic;
		std::string objectDiagnostic;
		std::string agentDiagnostic;
		std::string furnitureDiagnostic;
	};

	// Installs one object-shaped atlas - the Objects slot, or the Agent or
	// Furniture slot, which share the object-shaped loader but not the object
	// geometry - into the tileset `install` writes.
	//
	// `enforceObjectContract` asks the Objects contract of the set first: a set
	// that declares render semantics must be able to carry the Door, Window,
	// Lift and Shuttle geometry the object renderer draws from it (#1022). A
	// set that cannot is not installed at all, because installing it and
	// reporting afterwards would still draw a Door's artwork outside the
	// Door's collision. `mandatory` decides whether the failure also throws.
	void installObjectAtlasSlot(std::string const& setName,
		AtlasResource const& set, void (*install)(ObjectTileset, TextureId),
		std::string& diagnostic, bool mandatory, bool enforceObjectContract);

	// Installs one sector-shaped atlas into the Sector tileset, splitting the
	// manifest's `boundary-` prefixed names out into the tileset's own
	// boundary map the way the install convention says they are kept.
	void installSectorAtlasSlot(std::string const& setName,
		AtlasResource const& set, std::string& diagnostic);

	// Installs the four viewport atlases as the tilesets the producers read.
	// Every atlas goes through `installAtlasArtwork()`, so the two authorities
	// - the `ImageSet` for pixels, the authored tileset for render semantics -
	// are read the same way for all four.
	//
	// A missing atlas installs an empty tileset: every lookup then misses and
	// the draw helpers paint the cell opaque magenta, so a broken install is
	// visible rather than blank.
	//
	// `objectsAreMandatory` decides whether the Objects slot's failure throws
	// as well as being reported. At stack construction it throws: a World
	// cannot be drawn without an Objects atlas that can carry its geometry,
	// and starting up without one would be a half-drawn canvas. At a live
	// selection it does not, because the editor treats a bad artwork choice
	// as recoverable and reports it through the diagnostic instead of throwing
	// through the UI.
	void installAtlasDefinitions(ViewportAtlasSlots& slots,
		bool objectsAreMandatory);
}
