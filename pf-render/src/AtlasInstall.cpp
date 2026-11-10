// The shared renderer's atlas install path, lifted out of `WorldRenderStack` so
// a headless check can drive the same code the application runs (#1030). See
// include/AtlasInstall.h for what the two authorities are and why neither
// derives the other.

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

		// The authored tileset behind one atlas, read from the authored
		// atlas-set registry beside the application manifest. The manifest
		// cannot say this itself: the vendored Willpower `ImageSet` schema
		// fixes the Options an `ImageSet` is allowed to carry, and extending
		// the vendored SDK was rejected as a much wider blast radius than the
		// atlas work. The tileset stays the single authority for what a tile
		// means; the registry only says which tileset is asked about, and in
		// which authored format it is written.
		struct AuthoredTileset
		{
			AtlasAuthoredFormat format{};
			std::filesystem::path path{};
		};

		AuthoredTileset authoredTileset(resources::Resource const& resource)
		{
			auto const registry = std::filesystem::path(resource.getDefinitionFile())
				.parent_path() / "atlas-sets.yaml";
			auto uncovered = [&registry, &resource](std::string const& why)
			{
				return std::runtime_error(resource.getName() + " has no authored tileset: "
					+ why + " ('" + registry.string() + "')");
			};
			YAML::Node root;
			try { root = YAML::LoadFile(registry.string()); }
			catch (std::exception const& error)
			{
				throw std::runtime_error("Could not read the authored atlas-set registry '"
					+ registry.string() + "': " + error.what());
			}
			if (!root.IsMap() || !root["sets"].IsMap())
				throw std::runtime_error("The authored atlas-set registry '" + registry.string()
					+ "' has no `sets` map of atlas name to authored tileset");
			auto const entry = root["sets"][resource.getName()];
			if (!entry.IsMap()) throw uncovered("no map entry names it");
			auto const file = entry["tileset"].as<std::string>();
			if (file.empty()) throw uncovered("its tileset is empty");
			auto const kind = entry["kind"].as<std::string>();
			if (kind == "sector") return { AtlasAuthoredFormat::SectorTiles,
				registry.parent_path() / file };
			if (kind == "object") return { AtlasAuthoredFormat::ObjectTiles,
				registry.parent_path() / file };
			throw uncovered("its kind '" + kind + "' is neither 'sector' nor 'object'");
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
		};

		AuthoredAtlasTiles authoredTiles(resources::Resource const& resource,
			AtlasAuthoredFormat format)
		{
			auto const authored = authoredTileset(resource);
			if (authored.format != format)
				throw std::runtime_error(resource.getName() + " is installed as "
					+ authoredFormatName(format) + " but the atlas-set registry says "
					+ authoredFormatName(authored.format) + " ('"
					+ authored.path.string() + "')");
			if (format == AtlasAuthoredFormat::SectorTiles)
			{
				// The family has to come out with the tiles. The renderer
				// cannot recover it later: by then all it holds is a map of
				// regions, and a `shuttle` region says nothing about whether
				// the family it came from ever meant to draw it (#1027).
				auto set = SectorTileset::load(authored.path);
				return { std::move(set.tiles), false, set.family };
			}
			auto set = ObjectTileset::load(authored.path);
			return { std::move(set.sprites), set.declaresRenderSemantics };
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

	InstalledAtlas installAtlasArtwork(std::string const& setName,
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
		auto const authored = authoredTiles(*imageSet, format);
		return InstalledAtlas{ width, height,
			installAtlasTiles(authored.tiles, regions, width, height, setName),
			authored.declaresRenderSemantics, authored.family };
	}

	void installObjectAtlasSlot(std::string const& setName,
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

	void installSectorAtlasSlot(std::string const& setName,
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
			for (auto const& [name, artwork] : atlas.tiles)
			{
				// The manifest carries a sector atlas's boundary tiles
				// under a `boundary-` prefix; the tileset keeps them in
				// its own map under the bare name.
				if (name.rfind("boundary-", 0) == 0)
					tiles.boundaries.emplace(name.substr(9), artwork.region);
				else tiles.surfaces.emplace(name, artwork.region);
			}
		}
		catch (std::exception const& exception) { error = exception.what(); }
		setSectorTileset(std::move(tiles), kInstalledAtlasTexture);
		if (!error.empty() && diagnostic.empty()) diagnostic = error;
	}

	void installAtlasDefinitions(ViewportAtlasSlots& slots, bool objectsAreMandatory)
	{
		installSectorAtlasSlot(slots.sectorName, slots.sector,
			slots.sectorDiagnostic);
		installObjectAtlasSlot(slots.objectName, slots.objects, setObjectTileset,
			slots.objectDiagnostic, objectsAreMandatory,
			/*enforceObjectContract=*/ true);
		installObjectAtlasSlot(slots.agentName, slots.agent, setAgentTileset,
			slots.agentDiagnostic, /*mandatory=*/ false,
			/*enforceObjectContract=*/ false);
		installObjectAtlasSlot(slots.furnitureName, slots.furniture,
			setFurnitureTileset, slots.furnitureDiagnostic, /*mandatory=*/ false,
			/*enforceObjectContract=*/ false);
	}
}
