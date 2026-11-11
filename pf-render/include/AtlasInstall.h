#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
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
// green behind it. `WorldRenderStack` now holds a `ViewportAtlasSlots` and
// installs through `AtlasInstaller` exactly as a check does, so there is no
// caller left that can drift away from the path under test.
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
		// How many variants each base surface declared (#1038). Carried with
		// the artwork for the same reason the family is: the count is authored
		// in the tileset, and by the time the install is over a map of regions
		// says nothing about what was declared above them. Empty for every set
		// that derives no families, which is the honest answer rather than a
		// missing one.
		std::map<std::string, int> variantCounts;
		// The names the authored sector set put in its `overlays` map
		// (#1054). The `ImageSet` has no kind column - every entry it declares
		// is just a named pixel region - so which of them are doorframes and
		// which are Sector surfaces is a fact only the authored tileset holds.
		// The Sector slot routes them apart with this; every other slot reads
		// nothing from it.
		std::set<std::string> overlayNames;
	};

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

	// The authored tileset one atlas installs from: the file the registry
	// names for it, resolved against the registry's own directory, and the
	// authored format that file is written in.
	struct AuthoredAtlasSet
	{
		AtlasAuthoredFormat format{};
		std::filesystem::path path{};
	};

	// The atlas-set registry (`atlas-sets.yaml`) as a parsed value (#1035).
	//
	// The registry is small, static while the application runs, and asked on
	// every atlas install. Reading it off disk once per atlas per install was
	// what #1035 named: eight YAML reads for one artwork switch, four of them
	// the same registry file. Parsing it once into this type is what removes
	// that. The value is owned by whoever parses it - one per render stack,
	// held by the `AtlasInstaller` below - rather than cached process-wide,
	// so there is no invalidation rule to keep and no lifetime question to
	// answer.
	//
	// An entry is kept as the two strings the file writes rather than as a
	// resolved set. An entry that names no tileset, or a kind nothing loads,
	// is that one atlas's problem: refusing the whole registry over it would
	// take the three slots that work down with it, which is a worse failure
	// than the one being reported.
	class AtlasSetRegistry
	{
	public:
		struct Entry
		{
			// `sector` or `object`, as written.
			std::string kind;
			// The tileset file, relative to the registry's own directory.
			std::string tileset;
		};

		// Parses `path`. Throws `std::runtime_error` when the file cannot be
		// read, or when it carries no `sets` map of atlas name to entry.
		static AtlasSetRegistry load(std::filesystem::path const& path);

		// The file this was parsed from, named in every refusal this raises.
		std::filesystem::path const& path() const { return mPath; }

		// The entry written for one atlas name, or nullptr when the registry
		// writes none for it - including when it writes something that is not
		// a map, which is no entry as far as the install is concerned.
		Entry const* find(std::string const& atlasName) const;

		// How many atlases the registry writes an entry for.
		std::size_t size() const { return mEntries.size(); }

		// The authored set `atlasName`'s entry resolves to. Throws
		// `std::runtime_error` naming `atlasName` when no entry names it, when
		// its tileset is empty, or when its kind is neither `sector` nor
		// `object`.
		AuthoredAtlasSet require(std::string const& atlasName) const;

	private:
		std::filesystem::path mPath;
		std::map<std::string, Entry> mEntries;
	};

	// What one `AtlasInstaller` has read from disk since it was made (#1035).
	//
	// This is the instrumented seam the install checks read. "The registry is
	// parsed once per stack" and "selecting one atlas re-reads only that
	// atlas's tileset" are claims about how many files the install touches,
	// and with no count they can only be eyeballed - which is exactly how
	// eight reads per switch stayed invisible. Counts are kept per install
	// attempt rather than per success: a read that failed still cost the read.
	struct AtlasInstallReads
	{
		// Reads of the atlas-set registry itself.
		std::size_t registryParses{};
		// Reads of authored tileset files, in total and per atlas name. A
		// slot install that re-read a tileset it did not change shows up as
		// a count that moved when nothing of that slot's did.
		std::size_t tilesetLoads{};
		std::map<std::string, std::size_t> tilesetLoadsByAtlas;
	};

	// The install path with its caches: one per render stack (#1035).
	//
	// The installer holds the atlas-set registry parsed once and the read
	// counts, and it exposes each viewport slot as its own install entry
	// point, so a selection change installs the slot that changed instead of
	// walking all four. Every install still goes through `installAtlasArtwork`
	// - the two authorities are read the same way for all four slots - but
	// only for the slot that was asked for.
	class AtlasInstaller
	{
	public:
		// The registry that governs `resource`: the `atlas-sets.yaml` beside
		// the manifest that declared it. Parsed the first time it is asked
		// for and reused by every later install through this installer.
		// Throws `std::runtime_error` when it cannot be read.
		AtlasSetRegistry const& registryFor(
			wp::application::resourcesystem::Resource const& resource);

		// The pairing half of the install path, and the only place a drawn
		// region is chosen.
		//
		// Pixel regions come from the atlas's `ImageSet` resource - the
		// runtime pixel-region contract ADR 0010 names, and the only place a
		// drawn region is taken from. The render semantics that go with each
		// tile come from the authored tileset that the atlas-set registry
		// names for this atlas, under the `tileset` option. Nothing here
		// derives a tint from a tile's name or a drawn size from a domain
		// object's geometry.
		//
		// The pairing itself is done by `installAtlasTiles()`: it walks the
		// `ImageSet`'s regions and takes only the render semantics from the
		// tileset. A region that escapes the atlas image, and a tile the
		// `ImageSet` declares that the tileset never authored, are both
		// refused there rather than installed as artwork that samples outside
		// the sheet (#1025).
		//
		// Throws `std::runtime_error` when `set` is not a loadable `ImageSet`,
		// when the atlas-set registry does not name an authored tileset of
		// `format` for it, or when the two cannot be paired.
		InstalledAtlas installAtlasArtwork(std::string const& setName,
			AtlasResource const& set, AtlasAuthoredFormat format);

		// Installs one object-shaped atlas - the Objects slot, or the Agent or
		// Furniture slot, which share the object-shaped loader but not the
		// object geometry - into the tileset `install` writes.
		//
		// `enforceObjectContract` asks the Objects contract of the set first:
		// a set that declares render semantics must be able to carry the
		// Door, Window, Lift and Shuttle geometry the object renderer draws
		// from it (#1022). A set that cannot is not installed at all, because
		// installing it and reporting afterwards would still draw a Door's
		// artwork outside the Door's collision. `mandatory` decides whether
		// the failure also throws.
		void installObjectSlot(std::string const& setName,
			AtlasResource const& set, void (*install)(ObjectTileset, TextureId),
			std::string& diagnostic, bool mandatory, bool enforceObjectContract);

		// Installs one sector-shaped atlas into the Sector tileset, splitting
		// the manifest's `boundary-` prefixed names out into the tileset's own
		// boundary map the way the install convention says they are kept.
		void installSectorSlot(std::string const& setName, AtlasResource const& set,
			std::string& diagnostic);

		// Installs the four viewport atlases as the tilesets the producers
		// read, which is what a stack does at start-up. A selection change is
		// not this: it installs one slot.
		//
		// A missing atlas installs an empty tileset: every lookup then misses
		// and the draw helpers paint the cell opaque magenta, so a broken
		// install is visible rather than blank.
		//
		// `objectsAreMandatory` decides whether the Objects slot's failure
		// throws as well as being reported. At stack construction it throws:
		// a World cannot be drawn without an Objects atlas that can carry its
		// geometry, and starting up without one would be a half-drawn canvas.
		// At a live selection it does not, because the editor treats a bad
		// artwork choice as recoverable and reports it through the diagnostic
		// instead of throwing through the UI.
		void installAll(ViewportAtlasSlots& slots, bool objectsAreMandatory);

		// Everything this installer has read from disk. The seam #1035's
		// checks are asserted through.
		AtlasInstallReads const& reads() const { return mReads; }

	private:
		// The registry, parsed on first use and shared by every install this
		// installer makes.
		std::shared_ptr<AtlasSetRegistry const> mRegistry;
		AtlasInstallReads mReads;
	};
}
