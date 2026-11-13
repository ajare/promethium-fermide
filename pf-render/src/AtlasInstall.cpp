// The shared renderer's atlas install path, lifted out of `WorldRenderStack` so
// a headless check can drive the same code the application runs (#1030). See
// include/AtlasInstall.h for what the two authorities are and why neither
// derives the other.
//
// The registry read is parsed once per installer and every slot install goes
// through the parsed value (#1035); the read counts on the installer are what
// lets a check pin that, rather than leaving it to be read off this file.

#include "AtlasInstall.h"

#include <stdexcept>
#include <string>
#include <utility>

#include <willpower/application/resourcesystem/ImageResource.h>
#include <willpower/application/resourcesystem/ImageSetResource.h>
#include <willpower/application/resourcesystem/Resource.h>
#include <yaml-cpp/yaml.h>

#include "ObjectTileset.h"
#include "SectorTileset.h"
#include "TileArtwork.h"

namespace pf::render
{
	namespace
	{
		namespace resources = wp::application::resourcesystem;

		// Every installed atlas draws with this handle. The shared renderer
		// only separates "an atlas is installed" from "nothing is".
		constexpr TextureId kInstalledAtlasTexture{ 1 };

		// The registry file that governs a resource: the `atlas-sets.yaml`
		// beside the manifest that declared it.
		//
		// The manifest cannot say which tileset an atlas is installed from
		// itself: the vendored Willpower `ImageSet` schema fixes the Options
		// an `ImageSet` is allowed to carry, and extending the vendored SDK
		// was rejected as a much wider blast radius than the atlas work. The
		// tileset stays the single authority for what a tile means; the
		// registry only says which tileset is asked about, and in which
		// authored format it is written.
		std::filesystem::path registryPathFor(resources::Resource const& resource)
		{
			return std::filesystem::path(resource.getDefinitionFile())
				.parent_path() / "atlas-sets.yaml";
		}

		char const* authoredFormatName(AtlasAuthoredFormat format)
		{
			return format == AtlasAuthoredFormat::SectorTiles ? "sector tiles"
				: "object tiles";
		}

		// The authored render semantics behind one atlas. The object-shaped
		// atlases and the sector atlas are different authored formats, so the
		// registry declares which loader applies; both produce the same map
		// the install pairs against. The declared format is checked against
		// the slot being installed, so a mismatch is reported rather than
		// silently loading the wrong shape.
		struct AuthoredAtlasTiles
		{
			TileArtworkMap tiles;
			bool declaresRenderSemantics{ false };
			// The family the sector loader read out of the file's `version`.
			// Only the sector format has one that means anything; the object
			// branch leaves it at the Old family because nothing downstream
			// asks an object atlas which family it is (#1027).
			SectorTilesetFamily family{ SectorTilesetFamily::Old };
			// The variant counts the loader read off its base sprites (#1038,
			// and off the object format's base sprites since #1053). A set that
			// declares no families holds an empty map, which is the honest
			// answer rather than a missing one.
			std::map<std::string, int> variantCounts;
			// The names the sector file authored under `overlays` rather than
			// `sprites` (#1054). They travel as names, not as regions, because
			// the install already pairs the regions through `tiles`; all the
			// Sector slot needs is to know which names to route into its
			// `overlays` map instead of its `surfaces` one.
			std::set<std::string> overlayNames;
		};

		// Reads one atlas's authored tiles out of the tileset file its
		// registry entry names, counting the read against `reads` on the way
		// in. This is the only place the install path opens an authored tileset,
		// so the per-atlas count is the whole story (#1035).
		AuthoredAtlasTiles authoredTiles(resources::Resource const& resource,
			AtlasAuthoredFormat format, AtlasSetRegistry const& registry,
			AtlasInstallReads& reads)
		{
			auto const authored = registry.require(resource.getName());
			if (authored.format != format)
				throw std::runtime_error(resource.getName() + " is installed as "
					+ authoredFormatName(format) + " but the atlas-set registry says "
					+ authoredFormatName(authored.format) + " ('"
					+ authored.path.string() + "')");
			++reads.tilesetLoads;
			++reads.tilesetLoadsByAtlas[resource.getName()];
			if (format == AtlasAuthoredFormat::SectorTiles)
			{
				// The family has to come out with the tiles. The renderer
				// cannot recover it later: by then all it holds is a map of
				// regions, and a `shuttle` region says nothing about whether
				// the family it came from ever meant to draw it (#1027). The
				// variant counts travel for the same reason - a derived
				// `corridor-V2` region does not say that `corridor` declared
				// three of them, and the editor sizes its swatch strip from
				// that number rather than from a constant (#1038).
				auto set = SectorTileset::load(authored.path);
				// Which names are overlays travels as a set of names rather
				// than as the overlay map itself: the install pairs the
				// regions through `tiles`, and all the Sector slot has to
				// decide is which side of the `surfaces` line each name
				// falls on (#1054).
				std::set<std::string> overlayNames;
				for (auto const& entry : set.overlays)
					overlayNames.insert(entry.first);
				return { std::move(set.tiles), false, set.family,
					std::move(set.variantCounts), std::move(overlayNames) };
			}
			auto set = ObjectTileset::load(authored.path);
			AuthoredAtlasTiles result;
			result.tiles = std::move(set.sprites);
			result.declaresRenderSemantics = set.declaresRenderSemantics;
			// The variant counts travel back the same way they do for the
			// sector format. A derived `door-V2` region does not say that
			// `door` declared two of them, and the count is the author's
			// statement rather than something the installed region map can be
			// asked (#1053). A set that derives no families answers empty, which
			// is the honest answer rather than a missing one.
			result.variantCounts = std::move(set.variantCounts);
			return result;
		}

		// The pixel regions one atlas's `ImageSet` declares, with the image
		// size they are cut from.
		AtlasPixelRegions atlasPixelRegions(resources::ImageSetResource& set,
			int& imageWidth, int& imageHeight)
		{
			auto image = std::dynamic_pointer_cast<resources::ImageResource>(
				set.getImage());
			if (!image) throw std::runtime_error(set.getName()
				+ " has no Image resource to cut its regions from");
			imageWidth = image->getWidth();
			imageHeight = image->getHeight();
			AtlasPixelRegions regions;
			for (auto const& [name, definition] : set.getImageDefinitions())
				regions.emplace(name, AtlasPixelRegion{ static_cast<int>(definition.x),
					static_cast<int>(definition.y), static_cast<int>(definition.width),
					static_cast<int>(definition.height) });
			return regions;
		}
	}

	// ------------------------------------------------------------------
	// The atlas-set registry, parsed once (#1035).
	// ------------------------------------------------------------------

	AtlasSetRegistry AtlasSetRegistry::load(std::filesystem::path const& path)
	{
		AtlasSetRegistry registry;
		registry.mPath = path;
		YAML::Node root;
		try { root = YAML::LoadFile(path.string()); }
		catch (std::exception const& error)
		{
			throw std::runtime_error("Could not read the authored atlas-set registry '"
				+ path.string() + "': " + error.what());
		}
		// A node the file never wrote is an invalid node, and asking it
		// anything throws. Every read below is guarded on the node first, so
		// an entry that says nothing refuses as "it says nothing" rather than
		// as a conversion error nobody can act on. Reading through a const
		// handle is what keeps a missing key a missing key rather than an
		// inserted one.
		YAML::Node const& document = root;
		auto const sets = document["sets"];
		if (!document.IsMap() || !sets || !sets.IsMap())
			throw std::runtime_error("The authored atlas-set registry '" + path.string()
				+ "' has no `sets` map of atlas name to authored tileset");
		for (auto const& entry : sets)
		{
			// A key that is not a scalar cannot name an atlas, and throwing over
			// one would take every slot down with it. Skipping it leaves that
			// atlas unnamed, which is the refusal the install already reports.
			if (!entry.first.IsScalar()) continue;
			auto const name = entry.first.as<std::string>();
			// Something that is not a map is no entry: the install reads a
			// missing entry and a malformed one the same way, as a refusal to
			// name a tileset for that atlas.
			if (!entry.second.IsMap()) continue;
			auto const kind = entry.second["kind"];
			auto const tileset = entry.second["tileset"];
			Entry written;
			if (kind && kind.IsScalar()) written.kind = kind.as<std::string>();
			if (tileset && tileset.IsScalar())
				written.tileset = tileset.as<std::string>();
			registry.mEntries.emplace(name, std::move(written));
		}
		return registry;
	}

	AtlasSetRegistry::Entry const* AtlasSetRegistry::find(
		std::string const& atlasName) const
	{
		auto const entry = mEntries.find(atlasName);
		return entry == mEntries.end() ? nullptr : &entry->second;
	}

	AuthoredAtlasSet AtlasSetRegistry::require(std::string const& atlasName) const
	{
		auto const uncovered = [&atlasName, this](std::string const& why)
		{
			return std::runtime_error(atlasName + " has no authored tileset: "
				+ why + " ('" + mPath.string() + "')");
		};
		auto const entry = find(atlasName);
		if (!entry) throw uncovered("no map entry names it");
		if (entry->tileset.empty()) throw uncovered("its tileset is empty");
		if (entry->kind == "sector") return { AtlasAuthoredFormat::SectorTiles,
			mPath.parent_path() / entry->tileset };
		if (entry->kind == "object") return { AtlasAuthoredFormat::ObjectTiles,
			mPath.parent_path() / entry->tileset };
		throw uncovered("its kind '" + entry->kind + "' is neither 'sector' nor 'object'");
	}

	// ------------------------------------------------------------------
	// The installed Sector sheets, keyed by atlas name (#1087).
	// ------------------------------------------------------------------

	void SectorSheetRegistry::install(std::string const& name,
		SectorTileset tileset, TextureId texture)
	{
		InstalledSectorSheet sheet;
		sheet.name = name;
		sheet.tileset = std::move(tileset);
		sheet.texture = texture;
		// Replaced whole, never merged into what was there. A region left over
		// from the sheet this one displaces would draw as artwork nobody
		// installed, which is the one thing a keyed install cannot leave
		// behind: the key says which sheet is being drawn, so everything the
		// key resolves to has to have come from this install.
		mSheets[name] = std::move(sheet);
		mDrawingName = name;
	}

	InstalledSectorSheet const* SectorSheetRegistry::find(
		std::string const& name) const
	{
		auto const sheet = mSheets.find(name);
		return sheet == mSheets.end() ? nullptr : &sheet->second;
	}

	InstalledSectorSheet const* SectorSheetRegistry::drawing() const
	{
		return find(mDrawingName);
	}

	void SectorSheetRegistry::clear()
	{
		mSheets.clear();
		mDrawingName.clear();
	}

	SectorSheetRegistry& sectorSheetRegistry()
	{
		// Made on first use rather than at static initialisation. Every
		// translation unit that draws a Sector tile reaches this, and a
		// function-local makes what order it is built in a non-question.
		static SectorSheetRegistry registry;
		return registry;
	}

	// ------------------------------------------------------------------
	// The install path, with its registry cached (#1035).
	// ------------------------------------------------------------------

	AtlasSetRegistry const& AtlasInstaller::registryFor(
		resources::Resource const& resource)
	{
		if (!mRegistry)
		{
			++mReads.registryParses;
			mRegistry = std::make_shared<AtlasSetRegistry const>(
				AtlasSetRegistry::load(registryPathFor(resource)));
		}
		return *mRegistry;
	}

	InstalledAtlas AtlasInstaller::installAtlasArtwork(std::string const& setName,
		AtlasResource const& set, AtlasAuthoredFormat format)
	{
		auto const imageSet = std::dynamic_pointer_cast<
			resources::ImageSetResource>(set);
		if (!imageSet)
			throw std::runtime_error(setName
				+ " is not an installed ImageSet resource, so its atlas has no "
				"pixel regions to install");
		int width{}, height{};
		auto const regions = atlasPixelRegions(*imageSet, width, height);
		auto const authored = authoredTiles(*imageSet, format,
			registryFor(*imageSet), mReads);
		return InstalledAtlas{ width, height,
			installAtlasTiles(authored.tiles, regions, width, height, setName),
			authored.declaresRenderSemantics, authored.family,
			std::move(authored.variantCounts), std::move(authored.overlayNames) };
	}

	void AtlasInstaller::installObjectSlot(std::string const& setName,
		AtlasResource const& set, void (*install)(ObjectTileset, TextureId),
		std::string& diagnostic, bool mandatory, bool enforceObjectContract)
	{
		ObjectTileset tiles;
		std::string error;
		try
		{
			auto const atlas = installAtlasArtwork(setName, set,
				AtlasAuthoredFormat::ObjectTiles);
			tiles.width = atlas.width;
			tiles.height = atlas.height;
			tiles.sprites = atlas.tiles;
			tiles.declaresRenderSemantics = atlas.declaresRenderSemantics;
			// So do the variant counts, because a derived sprite says nothing
			// about the base that declared it (#1053).
			tiles.variantCounts = atlas.variantCounts;
		}
		catch (std::exception const& exception) { error = exception.what(); }
		if (enforceObjectContract && tiles.declaresRenderSemantics && error.empty())
			error = checkObjectAtlasContract(setName, tiles.sprites);
		// A set that cannot carry the object geometry is not installed at
		// all. Installing it and reporting the problem afterwards would
		// still draw a Door's artwork outside the Door's collision, which
		// is the exact failure this check exists to prevent; the slot goes
		// empty and draws magenta instead.
		if (!error.empty() && enforceObjectContract) tiles = {};
		install(std::move(tiles), kInstalledAtlasTexture);
		if (!error.empty() && (mandatory || diagnostic.empty())) diagnostic = error;
		if (mandatory && !error.empty()) throw std::runtime_error(error);
	}

	void AtlasInstaller::installSectorSlot(std::string const& setName,
		AtlasResource const& set, std::string& diagnostic)
	{
		SectorTileset tiles;
		std::string error;
		try
		{
			auto const atlas = installAtlasArtwork(setName, set,
				AtlasAuthoredFormat::SectorTiles);
			tiles.width = atlas.width;
			tiles.height = atlas.height;
			// The family travels with the artwork, because the Shuttle and
			// the Staircase choose their surface from it and from nothing
			// else (#1027).
			tiles.family = atlas.family;
			// So do the variant counts, because the per-type count is a fact
			// about the authored set and not something the installed region
			// map can be asked (#1038).
			tiles.variantCounts = std::move(atlas.variantCounts);
			for (auto const& [name, artwork] : atlas.tiles)
			{
				// The manifest carries a sector atlas's boundary tiles
				// under a `boundary-` prefix; the tileset keeps them in
				// its own map under the bare name.
				if (name.rfind("boundary-", 0) == 0)
					tiles.boundaries.emplace(name.substr(9), artwork.region);
				// A doorframe is a tile of this atlas but not a Sector
				// surface, and the query that answers "is this a Sector
				// surface" is asked about every name this loop could put
				// anywhere. The authored file said which names are frames;
				// routing them apart is the whole of what that statement
				// was for (#1054).
				else if (atlas.overlayNames.count(name) > 0)
					tiles.overlays.emplace(name, artwork.region);
				else tiles.surfaces.emplace(name, artwork.region);
			}
		}
		catch (std::exception const& exception) { error = exception.what(); }
		// The sheet goes into the keyed registry under the name it was asked
		// to install as, and becomes the sheet the draw path draws. A refused
		// install gets stored the same way, empty: the reason is the
		// diagnostic below, and the empty sheet is what the canvas then
		// paints magenta for (#1087).
		sectorSheetRegistry().install(setName, std::move(tiles),
			kInstalledAtlasTexture);
		if (!error.empty() && diagnostic.empty()) diagnostic = error;
	}

	void AtlasInstaller::installAll(ViewportAtlasSlots& slots,
		bool objectsAreMandatory)
	{
		installSectorSlot(slots.sectorName, slots.sector, slots.sectorDiagnostic);
		installObjectSlot(slots.objectName, slots.objects, setObjectTileset,
			slots.objectDiagnostic, objectsAreMandatory,
			/*enforceObjectContract=*/ true);
		installObjectSlot(slots.agentName, slots.agent, setAgentTileset,
			slots.agentDiagnostic, /*mandatory=*/ false,
			/*enforceObjectContract=*/ false);
		installObjectSlot(slots.furnitureName, slots.furniture,
			setFurnitureTileset, slots.furnitureDiagnostic, /*mandatory=*/ false,
			/*enforceObjectContract=*/ false);
	}
}
