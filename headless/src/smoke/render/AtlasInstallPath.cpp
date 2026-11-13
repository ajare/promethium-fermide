// AtlasInstallPath drives the shared renderer's atlas install path - the
// `AtlasInstaller::installAtlasArtwork()` / `installAll()` pair the
// application actually runs at start-up, and the per-slot install every
// atlas selection runs (#1035) - against the manifest the application
// actually ships.
//
// Until #1030 every install assertion in this suite called `installAtlasTiles()`
// by hand. That proved the helper and left its caller untested, which is the
// systemic reason four separate defects stayed green: #1025 installed the
// tileset's own regions instead of the `ImageSet`'s, #1026 never selected the
// Objects atlas in the shipped game, #1028 shipped an Android type naming a
// tile the new Agent sheet never carried, and #1029 shipped Furniture
// catalogues that did the same against the new Furniture sheet. A hand-built
// tileset cannot see any of those, because the thing that was wrong is what
// sits between the manifest and the helper.
//
// So nothing below builds a tileset by hand. Each check stages a real
// `wp::application::resourcesystem::ResourceManager` over a real
// `Resources.yaml`, loads real `ImageSetResource`s through it, and asks the
// install path what it installed. The only thing missing is the render system,
// which the install path never asks for - that is why the install path lives
// outside `WorldRenderStack` rather than inside it.
//
// Assertions are on the installed value itself: the pixel region per tile,
// the render offset and the drawn size, and - for the four live
// viewport slots - what the renderer's own read surface reports back after the
// install has run.

#include "Checks.h"

#include "AtlasInstall.h"
#include "DrawTypes.h"
#include "LocationPlan.h"
#include "ObjectTileset.h"
#include "Render.h"
#include "SectorDrawSheet.h"
#include "SectorTileset.h"
#include "TileArtwork.h"
#include "UISettings.h"
#include "WorldDrawSegments.h"
#include "WorldRenderSlotAllocator.h"

#include "core/Agent.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Furniture.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/Window.h"
#include "core/WindowSectorObject.h"
#include "core/World.h"
#include "WorldDrawList.h"

#include <mpp/Logger.h>
#include <mpp/ResourceManager.h>
#include <willpower/application/resourcesystem/DirectoryResourceLocation.h>
#include <willpower/application/resourcesystem/ImageResource.h>
#include <willpower/application/resourcesystem/ImageSetResource.h>
#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/common/Logger.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern UISettings gUISettings;

namespace
{
	using smoke::require;
	namespace resources = wp::application::resourcesystem;

	std::string readFile(std::filesystem::path const& path)
	{
		std::ifstream file(path, std::ios::binary);
		require(bool(file), "Could not open " + path.string());
		return { std::istreambuf_iterator<char>(file),
			std::istreambuf_iterator<char>() };
	}

	// The PNG's own dimensions, read from its IHDR chunk. The install cuts its
	// regions against the decoded image, so the committed file has to agree
	// with whatever the tileset and manifest claim about its size.
	std::pair<int, int> pngSize(std::filesystem::path const& path)
	{
		std::ifstream file(path, std::ios::binary);
		require(bool(file), "Could not open the atlas image: " + path.string());
		unsigned char header[24]{};
		file.read(reinterpret_cast<char*>(header), sizeof(header));
		require(file.gcount() == static_cast<std::streamsize>(sizeof(header)),
			"The atlas image is too short to carry its header: " + path.string());
		auto const be = [&](std::size_t offset) -> unsigned {
			return (unsigned(header[offset]) << 24) | (unsigned(header[offset + 1]) << 16)
				| (unsigned(header[offset + 2]) << 8) | unsigned(header[offset + 3]);
		};
		require(be(0) == 0x89504e47u && be(12) == 0x49484452u,
			"The atlas image is not a PNG: " + path.string());
		return { int(be(16)), int(be(20)) };
	}

	// ------------------------------------------------------------------
	// The shipped manifest and the authored atlas-set registry.
	// ------------------------------------------------------------------

	struct ManifestEntry
	{
		std::string name;
		std::string type;
		// The image an `Image` resource points at, relative to the manifest's
		// own directory. Empty for anything that is not an `Image`.
		std::string location;
		// For an `ImageSet`, the `Image` resource it cuts its regions from.
		std::string imageReference;
		AtlasPixelRegions regions;
	};

	// Every resource the application asset manifest registers, keyed by name.
	// Each field is guarded on its own because a yaml-cpp node that was never
	// written is a zombie, and subscripting one throws rather than reading as
	// absent.
	std::map<std::string, ManifestEntry> manifestEntries(
		YAML::Node const& manifest)
	{
		std::map<std::string, ManifestEntry> entries;
		for (auto const& resource : manifest["Resources"]["Resource"])
		{
			if (!resource.IsMap()) continue;
			ManifestEntry entry;
			entry.name = resource["name"].as<std::string>();
			entry.type = resource["type"].as<std::string>();
			if (resource["location"])
				entry.location = resource["location"].as<std::string>();
			if (auto const dependencies = resource["DependentResources"])
			{
				auto const dependency = dependencies["DependentResource"];
				if (dependency.IsMap() && dependency["ref"])
					entry.imageReference = dependency["ref"].as<std::string>();
			}
			if (auto const definitions = resource["Definitions"])
				if (auto const definition = definitions["Definition"])
					if (auto const images = definition["Images"])
						for (auto const& image : images["Image"])
							entry.regions.emplace(image["name"].as<std::string>(),
								AtlasPixelRegion{ image["x"].as<int>(),
									image["y"].as<int>(), image["width"].as<int>(),
									image["height"].as<int>() });
			entries.emplace(entry.name, std::move(entry));
		}
		return entries;
	}

	struct RegistryEntry
	{
		std::string kind;
		std::string tileset;
	};

	// The authored atlas-set registry beside the manifest: which tileset is
	// asked about for which atlas, and in which authored format.
	std::map<std::string, RegistryEntry> atlasSetRegistry(
		smoke::Context const& context)
	{
		auto const root = YAML::LoadFile(
			context.fixture("resources/atlas-sets.yaml").string());
		require(root.IsMap() && root["sets"].IsMap(),
			"resources/atlas-sets.yaml has no `sets` map of atlas name to authored "
			"tileset");
		std::map<std::string, RegistryEntry> sets;
		for (auto const& entry : root["sets"])
			sets[entry.first.as<std::string>()] = RegistryEntry{
				entry.second["kind"].as<std::string>(),
				entry.second["tileset"].as<std::string>() };
		return sets;
	}

	// Every `ImageSet` the shipped manifest registers, in name order.
	std::vector<std::string> registeredImageSets(
		std::map<std::string, ManifestEntry> const& entries)
	{
		std::vector<std::string> names;
		for (auto const& [name, entry] : entries)
			if (entry.type == "ImageSet") names.push_back(name);
		return names;
	}

	// The authored render semantics one atlas's tileset declares, in the same
	// map shape the install pairs against.
	TileArtworkMap authoredTilesetTiles(smoke::Context const& context,
		RegistryEntry const& registry)
	{
		auto const path = context.fixture("resources/" + registry.tileset);
		if (registry.kind == "sector") return SectorTileset::load(path).tiles;
		return ObjectTileset::load(path).sprites;
	}

	// ------------------------------------------------------------------
	// A real resource manager over a real resource directory.
	// ------------------------------------------------------------------

	// The application's own resource manager, over `directory`, built the way
	// the application builds its own: the `Resources.yaml` location added, then
	// a scan. An atlas loaded through `load()` is created and loaded for real,
	// which decodes its image and resolves its `Image` dependency exactly as
	// the editor's start-up does.
	//
	// Two things are deliberately absent, and neither is anything the install
	// path asks for. The render system is one: the install pairs a decoded
	// `ImageSet` with an authored tileset file, and no GPU is involved in that.
	// The application-level resource registrations are the other - the
	// manifest declares `World`, `AgentType` and `FurnitureCatalogue` records
	// as well, but the install only ever resolves `ImageSet` and `Image`, and
	// the Willpower resource manager registers both of those itself. A record
	// whose type has no factory is refused at acquire time, not at scan time,
	// so leaving the application tier off this manager costs nothing here.
	class ResourceManagerOver
	{
	public:
		explicit ResourceManagerOver(std::filesystem::path directory)
			: mDirectory(std::move(directory))
			, mRenderResources(nullptr, &mRenderLogger)
			, mManager(std::make_unique<resources::ResourceManager>(nullptr,
				&mRenderResources, nullptr, &mLogger))
		{
			mManager->addResourceLocationFactory("Directory",
				[logger = &mLogger](std::string const& location,
					std::string const& definition) -> resources::ResourceLocation*
				{
					return new resources::DirectoryResourceLocation(logger,
						location, definition);
				});
			mManager->addResourceLocation("Directory", mDirectory.string(),
				"Resources.yaml");
			mManager->scanLocations();
		}

		ResourceManagerOver(ResourceManagerOver const&) = delete;
		ResourceManagerOver& operator=(ResourceManagerOver const&) = delete;

		// Creates and loads one registered atlas, the way the render stack's
		// own `requireImageSet` does.
		resources::ResourcePtr load(std::string const& name)
		{
			auto resource = mManager->acquireResource(name);
			require(resource != nullptr,
				"The resource manager acquired no '" + name + "' resource");
			mAcquired.push_back(resource);
			try
			{
				mManager->createResource(resource);
				mManager->loadResource(resource);
			}
			catch (std::exception const& error)
			{
				throw std::runtime_error("Could not load the atlas '" + name
					+ "': " + error.what());
			}
			require(std::dynamic_pointer_cast<resources::ImageSetResource>(resource)
				!= nullptr, "'" + name + "' did not load as an ImageSet resource");
			return resource;
		}

		~ResourceManagerOver()
		{
			// A check that threw part-way through still has to hand back what
			// it acquired, and a destructor that throws on the way out would
			// turn one failure into an abort.
			try
			{
				for (auto& resource : mAcquired) mManager->releaseResource(resource);
			}
			catch (std::exception const&) {}
		}

	private:
		std::filesystem::path mDirectory;
		mpp::Logger mRenderLogger;
		mpp::ResourceManager mRenderResources;
		wp::Logger mLogger;
		std::unique_ptr<resources::ResourceManager> mManager;
		std::vector<resources::ResourcePtr> mAcquired;
	};

	// The four viewport tilesets are process-global state, and every check
	// here installs into them. Each one hands them back empty on the way out,
	// so a later check can never pass on artwork it never asked for.
	struct ClearInstalledTilesets
	{
		~ClearInstalledTilesets()
		{
			clearSectorTileset();
			clearObjectTileset();
			clearAgentTileset();
			clearFurnitureTileset();
		}
	};

	// Copies one shipped `resources/` file into a staging directory at the
	// same relative path it holds in the bundle.
	void copyShipped(smoke::Context const& context,
		std::filesystem::path const& directory, std::string const& relative)
	{
		auto const destination = directory / relative;
		std::filesystem::create_directories(destination.parent_path());
		std::filesystem::copy_file(context.fixture("resources/" + relative),
			destination, std::filesystem::copy_options::overwrite_existing);
	}

	// ------------------------------------------------------------------
	// The shipped atlas records, staged for a real resource manager.
	// ------------------------------------------------------------------

	// A throwaway resource directory built out of the shipped manifest.
	//
	// The install path reads three things and nothing else: the `ImageSet`'s
	// pixel regions, the atlas-set registry beside the manifest, and the
	// authored tileset that registry names for the atlas. Every one of those
	// is staged here as a byte-for-byte copy of the shipped file, and the
	// staged `Resources.yaml` is the shipped manifest's own `Image` and
	// `ImageSet` records cloned out of it rather than rewritten by hand.
	//
	// What is left out is every record that is not an atlas - the shipped
	// `World`, `AgentType`, `AgentTagRegistry`, `AgentBehaviourRegistry`,
	// `FurnitureCatalogue` and `LuaScript` records. `scanLocations`
	// instantiates every record it reads, and the factories for those types
	// are compiled into the application tier, which this module deliberately
	// does not link: the headless tier's include roots stop at pf-render and
	// pf-agent-resources on purpose, and the shipped game's library has no
	// business standing behind a CPU-only atlas check. The install path
	// resolves none of them. Every sweep below insists each registered atlas
	// is present in the staged directory, so the filter cannot quietly drop
	// one.
	//
	// `driftAtlas`, when named, has its region list edited by
	// `mutateRegions` on the way in. The drift is made by moving a real
	// shipped entry by one field rather than by writing a fake one, so what
	// gets refused is the shipped atlas with one number changed.
	// `mutateRegions` is handed the region sequence by value because a
	// `YAML::Node` is a handle over shared node data: copying it is how
	// yaml-cpp spells "the same node".
	class StagedAtlasDirectory
	{
	public:
		StagedAtlasDirectory(smoke::Context const& context,
			std::string const& tag, std::vector<std::string> const& atlases,
			std::string const& driftAtlas = {},
			std::function<void(YAML::Node)> const& mutateRegions = {})
			: mDirectory(context.temporaryRoot() / tag)
		{
			auto const shipped = YAML::LoadFile(
				context.fixture("resources/Resources.yaml").string());
			auto const entries = manifestEntries(shipped);
			auto const registry = atlasSetRegistry(context);
			std::set<std::string> wanted(atlases.begin(), atlases.end());
			require(!wanted.empty(), "An atlas directory was staged with no atlases "
				"in it");
			require(driftAtlas.empty() || wanted.count(driftAtlas), "The drifted "
				"atlas '" + driftAtlas + "' was not among the atlases being staged, "
				"so the drift would have had nothing to edit");

			// Two passes, because the manifest declares each `Image` before
			// the `ImageSet` that points at it: the references have to be
			// collected before the records that satisfy them can be kept.
			std::set<std::string> imagesNeeded;
			for (auto const& name : wanted)
			{
				auto const atlas = entries.find(name);
				require(atlas != entries.end(), "The shipped manifest registers no "
					+ name + " resource");
				require(atlas->second.type == "ImageSet", "The shipped manifest's "
					+ name + " is a '" + atlas->second.type + "', not an `ImageSet`");
				require(registry.count(name), "The atlas-set registry names no "
					"authored tileset for " + name);
				require(!atlas->second.imageReference.empty(), "The shipped " + name
					+ " `ImageSet` references no `Image` resource");
				imagesNeeded.insert(atlas->second.imageReference);
			}

			auto staged = YAML::Node(YAML::NodeType::Map);
			auto records = YAML::Node(YAML::NodeType::Sequence);
			std::set<std::string> stagedAtlases;
			for (std::size_t index = 0; index < shipped["Resources"]["Resource"].size();
				++index)
			{
				auto const resource = shipped["Resources"]["Resource"][index];
				if (!resource.IsMap()) continue;
				auto const name = resource["name"].as<std::string>();
				auto const type = resource["type"].as<std::string>();
				if (type == "ImageSet" && wanted.count(name))
				{
					auto copy = YAML::Clone(resource);
					if (name == driftAtlas)
						mutateRegions(copy["Definitions"]["Definition"]["Images"]["Image"]);
					records.push_back(copy);
					stagedAtlases.insert(name);
				}
				else if (type == "Image" && imagesNeeded.count(name))
					records.push_back(YAML::Clone(resource));
			}
			require(stagedAtlases == wanted, "Staging dropped an atlas: asked for "
				+ std::to_string(wanted.size()) + ", staged "
				+ std::to_string(stagedAtlases.size()));
			staged["Resources"]["Resource"] = records;

			std::filesystem::create_directories(mDirectory);
			{
				std::ofstream out(mDirectory / "Resources.yaml");
				require(bool(out), "Could not write the staged manifest into "
					+ mDirectory.string());
				out << YAML::Dump(staged);
			}

			// The registry, the tilesets and the images the staged manifest
			// names, copied at the same relative paths so every lookup the
			// install makes resolves against the shipped bytes.
			std::filesystem::copy_file(context.fixture("resources/atlas-sets.yaml"),
				mDirectory / "atlas-sets.yaml",
				std::filesystem::copy_options::overwrite_existing);
			for (auto const& name : wanted)
				copyShipped(context, mDirectory, registry.at(name).tileset);
			for (auto const& name : imagesNeeded)
				copyShipped(context, mDirectory, entries.at(name).location);

			mResources = std::make_unique<ResourceManagerOver>(mDirectory);
		}

		resources::ResourcePtr load(std::string const& name) const
		{
			return mResources->load(name);
		}

	private:
		std::filesystem::path mDirectory;
		mutable std::unique_ptr<ResourceManagerOver> mResources;
	};

	// Runs one install and returns the thrown refusal's text, or an empty
	// string when the install succeeded. The installer is passed in rather
	// than made here because the caller's read counts are part of what the
	// checks below assert on (#1035).
	std::string installRefusal(pf::render::AtlasInstaller& installer,
		std::string const& setName, resources::ResourcePtr const& set,
		pf::render::AtlasAuthoredFormat format)
	{
		try
		{
			installer.installAtlasArtwork(setName, set, format);
			return {};
		}
		catch (std::exception const& error) { return error.what(); }
	}

	// How many times one atlas's authored tileset was read off disk. A
	// missing entry reads as zero rather than throwing, so a count that went
	// missing fails an assertion with a message instead of ending the check.
	std::size_t tilesetLoadsFor(pf::render::AtlasInstallReads const& reads,
		std::string const& atlas)
	{
		auto const entry = reads.tilesetLoadsByAtlas.find(atlas);
		return entry == reads.tilesetLoadsByAtlas.end() ? 0 : entry->second;
	}

	pf::render::AtlasAuthoredFormat registryFormat(std::string const& kind)
	{
		require(kind == "sector" || kind == "object", "The atlas-set registry gave "
			"a kind of '" + kind + "', which is neither 'sector' nor 'object'");
		return kind == "sector" ? pf::render::AtlasAuthoredFormat::SectorTiles
			: pf::render::AtlasAuthoredFormat::ObjectTiles;
	}

	std::string regionsText(SectorTileRegion const& region)
	{
		return std::to_string(region.x) + "," + std::to_string(region.y) + " "
			+ std::to_string(region.width) + "x" + std::to_string(region.height);
	}

	std::string regionsText(AtlasPixelRegion const& region)
	{
		return std::to_string(region.x) + "," + std::to_string(region.y) + " "
			+ std::to_string(region.width) + "x" + std::to_string(region.height);
	}

	// ------------------------------------------------------------------
	// AC: every registered `ImageSet` installs through the real path, and
	// what it installs is the manifest's pixels under the tileset's
	// semantics.
	// ------------------------------------------------------------------
	void everyRegisteredImageSetInstallsFromTheShippedManifest(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		auto const manifest = YAML::LoadFile(
			context.fixture("resources/Resources.yaml").string());
		auto const entries = manifestEntries(manifest);
		auto const registry = atlasSetRegistry(context);
		auto const atlases = registeredImageSets(entries);

		// AC: all eight registered `ImageSet`s - the four viewport atlases,
		// the three themed sheets, and the Layer-1 Sector copy (#1086). The
		// count is asserted rather than assumed so a ninth atlas is noticed
		// the day it is registered and paired here, and so a sheet dropping out
		// of the manifest fails here rather than going untested.
		require(atlases.size() == 8, "The shipped manifest registers "
			+ std::to_string(atlases.size()) + " `ImageSet`s; the install path is "
			"being asserted against " + std::to_string(atlases.size())
			+ ", and every atlas outside this set installs untested. The bundle "
			"ships the four viewport atlases, the three themed sheets and the "
			"Layer-1 Sector copy.");
		// AC: every registered atlas is covered by the atlas-set registry.
		for (auto const& name : atlases)
			require(registry.count(name), "The shipped manifest registers the "
				+ name + " `ImageSet` but resources/atlas-sets.yaml names no "
				"authored tileset for it, so the install has no render semantics "
				"to pair its pixels with");

		StagedAtlasDirectory staged{ context, "shipped-atlases", atlases };
		pf::render::AtlasInstaller installer;
		std::size_t tilesChecked{};
		for (auto const& name : atlases)
		{
			auto const set = staged.load(name);
			auto const entry = registry.at(name);
			auto const format = registryFormat(entry.kind);
			std::string refusal;
			pf::render::InstalledAtlas installed;
			try
			{
				installed = installer.installAtlasArtwork(name, set, format);
			}
			catch (std::exception const& error)
			{
				refusal = error.what();
			}
			require(refusal.empty(), "Installing the shipped " + name + " atlas "
				+ "through the render stack's own install path was refused: "
				+ refusal);

			auto const authored = authoredTilesetTiles(context, entry);
			auto const [imageWidth, imageHeight] = pngSize(
				context.fixture("resources/"
					+ entries.at(entries.at(name).imageReference).location));
			require(installed.width == imageWidth && installed.height == imageHeight,
				name + " installed as " + std::to_string(installed.width) + "x"
				+ std::to_string(installed.height) + " but its committed image is "
				+ std::to_string(imageWidth) + "x" + std::to_string(imageHeight));

			// The installed tile set is exactly the `ImageSet`'s region set:
			// nothing dropped, nothing invented.
			require(installed.tiles.size() == entries.at(name).regions.size(),
				name + " installed " + std::to_string(installed.tiles.size())
				+ " tiles but its `ImageSet` declares "
				+ std::to_string(entries.at(name).regions.size()) + " regions");

			for (auto const& [tile, artwork] : installed.tiles)
			{
				auto const who = "The " + name + " atlas tile '" + tile + "' ";
				auto const declared = entries.at(name).regions.find(tile);
				require(declared != entries.at(name).regions.end(), who
					+ "was installed without a region in the manifest, so the "
					"install invented a pixel region");
				// AC: the installed region is the `ImageSet` region.
				require(artwork.region.x == declared->second.x
					&& artwork.region.y == declared->second.y
					&& artwork.region.width == declared->second.width
					&& artwork.region.height == declared->second.height, who
					+ "installed as " + regionsText(artwork.region) + " but the "
					"manifest's `ImageSet` declares " + regionsText(declared->second)
					+ ", so the install is not taking its pixels from the "
					"`ImageSet`");
				auto const author = authored.find(tile);
				require(author != authored.end(), who + "has no entry in its "
					"authored tileset " + entry.tileset + ", so its render "
					"semantics came from nowhere");
				// AC: the installed render semantics are the authored ones.
				require(artwork.offset.x == author->second.offset.x
					&& artwork.offset.y == author->second.offset.y, who
					+ "installed at render offset " + std::to_string(artwork.offset.x)
					+ "," + std::to_string(artwork.offset.y) + " but "
					+ entry.tileset + " authors "
					+ std::to_string(author->second.offset.x) + ","
					+ std::to_string(author->second.offset.y));
				require(artwork.drawnSize.x == author->second.drawnSize.x
					&& artwork.drawnSize.y == author->second.drawnSize.y, who
					+ "installed with drawn size "
					+ std::to_string(artwork.drawnSize.x) + "x"
					+ std::to_string(artwork.drawnSize.y) + " but " + entry.tileset
					+ " authors " + std::to_string(author->second.drawnSize.x) + "x"
					+ std::to_string(author->second.drawnSize.y));
				++tilesChecked;
			}
		}
		require(tilesChecked > 100, "Only " + std::to_string(tilesChecked)
			+ " tiles were paired across the registered atlases; the "
			"bundle declares far more, so the sweep is stopping short");
	}

	// ------------------------------------------------------------------
	// AC (#1025): the installed region is the `ImageSet`'s, not the
	// authored tileset's own. Three shipped atlases make the two disagree,
	// so they are the only ones that can tell the two authorities apart.
	// ------------------------------------------------------------------
	void theInstalledRegionIsTheImageSetsNotTheTilesets(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		auto const manifest = YAML::LoadFile(
			context.fixture("resources/Resources.yaml").string());
		auto const entries = manifestEntries(manifest);
		auto const registry = atlasSetRegistry(context);
		StagedAtlasDirectory staged{ context, "pixel-authority",
			registeredImageSets(entries) };

		pf::render::AtlasInstaller installer;
		std::size_t disagreements{};
		std::vector<std::string> checkedAtlases;
		for (auto const& name : registeredImageSets(entries))
		{
			auto const entry = registry.at(name);
			auto const authored = authoredTilesetTiles(context, entry);
			auto const set = staged.load(name);
			auto const installed = installer.installAtlasArtwork(name, set,
				registryFormat(entry.kind));
			for (auto const& [tile, artwork] : installed.tiles)
			{
				auto const author = authored.find(tile);
				if (author == authored.end()) continue;
				if (author->second.region.x == artwork.region.x
					&& author->second.region.y == artwork.region.y
					&& author->second.region.width == artwork.region.width
					&& author->second.region.height == artwork.region.height)
					continue;
				// The two authorities disagree. The `ImageSet` has to have won.
				auto const declared = entries.at(name).regions.at(tile);
				require(artwork.region.x == declared.x
					&& artwork.region.y == declared.y
					&& artwork.region.width == declared.width
					&& artwork.region.height == declared.height, "The " + name
					+ " tile '" + tile + "' disagrees between its `ImageSet` ("
					+ regionsText(declared) + ") and its authored tileset ("
					+ regionsText(author->second.region) + "), and the installed "
					"region is neither, so the install is not honouring the "
					"`ImageSet` as the pixel authority");
				++disagreements;
				if (std::find(checkedAtlases.begin(), checkedAtlases.end(), name)
					== checkedAtlases.end()) checkedAtlases.push_back(name);
			}
		}
		// The discriminator has to actually fire. If every atlas agreed, this
		// check would be indistinguishable from the one above and #1025 could
		// come back unnoticed.
		require(disagreements >= 3, "Only " + std::to_string(disagreements)
			+ " shipped tiles disagree between their `ImageSet` region and their "
			"authored tileset's own region. The themed sheets are what make the "
			"two authorities distinguishable; with nothing disagreeing, an install "
			"that took the tileset's regions would pass every check here.");
		require(checkedAtlases.size() >= 2, "Only " + std::to_string(
			checkedAtlases.size()) + " atlases disagree at all; the disagreement "
			"is supposed to be spread across the themed sheets, not concentrated");
	}

	// ------------------------------------------------------------------
	// AC: a deliberately drifted `ImageSet` region escaping the image is
	// refused rather than rendered.
	//
	// The Willpower `ImageSet` validates every region against the image it
	// was cut from, so a region pushed off the sheet stops the atlas from
	// becoming a loadable resource at all - nothing downstream can draw it.
	// The install keeps its own bounds check behind this one as defence in
	// depth against a region that reaches it some other way; that check is
	// asserted at the helper by `walkwayArtwork/installSeparatesPixelsFromSemantics`.
	// What this check owns is the shipped path: the drift is made in a copy
	// of the real manifest, and the pipeline refuses it before the install
	// is ever asked.
	// ------------------------------------------------------------------
	void aRegionEscapingTheAtlasImageIsNeverLoadable(smoke::Context const& context)
	{
		auto const manifest = YAML::LoadFile(
			context.fixture("resources/Resources.yaml").string());
		auto const entries = manifestEntries(manifest);
		auto const image = entries.at(entries.at("SectorAtlas").imageReference);
		auto const [imageWidth, imageHeight] = pngSize(
			context.fixture("resources/" + image.location));

		// The `room` tile, pushed one whole image height down. It is still a
		// real, authored, positively-sized tile; the only thing wrong with it
		// is that it no longer lies on the sheet.
		StagedAtlasDirectory offImage{ context, "off-image-drift",
			{ "SectorAtlas" }, "SectorAtlas", [&](YAML::Node regions)
			{
				require(regions.IsSequence() && regions.size() > 0,
					"SectorAtlas declares no regions to drift");
				for (std::size_t index = 0; index < regions.size(); ++index)
				{
					auto region = regions[index];
					if (region["name"].as<std::string>() != "room") continue;
					region["y"] = std::to_string(imageHeight);
				}
			} };
		std::string loadRefusal;
		try { offImage.load("SectorAtlas"); }
		catch (std::exception const& error) { loadRefusal = error.what(); }
		require(!loadRefusal.empty(), "A `room` region pushed off the bottom of "
			"the atlas image loaded cleanly, so an escaping region reached the "
			"install path instead of being stopped at the resource");
		require(loadRefusal.find("room") != std::string::npos, "The refusal does "
			"not name the tile that escaped: " + loadRefusal);
	}

	// ------------------------------------------------------------------
	// AC: a manifest that names a tile its authored tileset never defines
	// is refused rather than installed.
	// ------------------------------------------------------------------
	void aManifestTileTheAuthoredTilesetNeverDefinesIsRefused(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		StagedAtlasDirectory staged{ context, "undefined-tile-drift",
			{ "SectorAtlas" }, "SectorAtlas", [](YAML::Node regions)
			{
				auto added = YAML::Node(YAML::NodeType::Map);
				added["name"] = "tile-nobody-authored";
				added["x"] = "0";
				added["y"] = "0";
				added["width"] = "16";
				added["height"] = "16";
				regions.push_back(added);
			} };
		auto const set = staged.load("SectorAtlas");

		pf::render::AtlasInstaller installer;
		auto const refusal = installRefusal(installer, "SectorAtlas", set,
			pf::render::AtlasAuthoredFormat::SectorTiles);
		require(!refusal.empty(), "The manifest gained a tile its authored "
			"tileset never defines and the install accepted it, so a tile with no "
			"declared visual extent would have been installed");
		require(refusal.find("tile-nobody-authored") != std::string::npos,
			"The refusal does not name the undefined tile: " + refusal);
		require(refusal.find("never defines") != std::string::npos, "The refusal "
			"does not say the authored tileset never defines it: " + refusal);

		// And the refusal reaches the slot the application would have drawn,
		// rather than only being visible to whoever called the helper. A
		// refusal nobody reports is a refusal that still renders.
		pf::render::ViewportAtlasSlots slots;
		slots.sectorName = "SectorAtlas";
		slots.sector = set;
		pf::render::AtlasInstaller{}.installAll(slots,
			/*objectsAreMandatory=*/ false);
		require(!slots.sectorDiagnostic.empty(), "installAll left the "
			"Sector slot's diagnostic empty over an atlas the install refused, so "
			"the application would draw the bad atlas and report nothing");
		require(slots.sectorDiagnostic.find("tile-nobody-authored")
			!= std::string::npos, "The Sector diagnostic does not carry the "
			"refusal: " + slots.sectorDiagnostic);
		require(!hasSectorTileSurface(drawingSectorSheet(), "room"), "The refused Sector atlas still "
			"installed its other surfaces, so the refusal did not stop the install");
	}

	// The four viewport atlases a new World defaults to, named once so every
	// check that installs the quadruple installs the same four. Since the
	// #1013 terminus dropped the `New` suffix these are the canonical names,
	// and they are the sheets sized for the 128x320 World cell.
	std::vector<std::string> const kViewportAtlases{ "SectorAtlas",
		"ObjectAtlas", "AgentAtlas", "FurnitureAtlas" };

	// ------------------------------------------------------------------
	// AC: the other direction of the same drift. The four viewport atlases
	// are generated from their tilesets, so their region lists and their
	// tilesets' tile lists are supposed to be the same list. A tileset that
	// grows a tile the manifest never got is an authored tile nothing
	// draws, and an atlas the manifest trimmed is a tile the renderer will
	// look up and not find.
	// ------------------------------------------------------------------
	void everyViewportAtlasMatchesItsTilesetNameForName(
		smoke::Context const& context)
	{
		auto const manifest = YAML::LoadFile(
			context.fixture("resources/Resources.yaml").string());
		auto const entries = manifestEntries(manifest);
		auto const registry = atlasSetRegistry(context);

		std::vector<std::string> atlases;
		for (auto const& name : kViewportAtlases)
			if (registry.count(name)) atlases.push_back(name);
		require(atlases.size() >= 4, "The atlas-set registry names only "
			+ std::to_string(atlases.size()) + " of the four viewport atlases; "
			"the bundle ships four, so there is too little here to pair");

		for (auto const& name : atlases)
		{
			require(entries.count(name), "The atlas-set registry names " + name
				+ " but the shipped manifest registers no such `ImageSet`, so "
				"nothing will ever load the tileset this entry points at");
			auto const authored = authoredTilesetTiles(context, registry.at(name));
			auto const& declared = entries.at(name).regions;
			// The example tile is read out of the vector before `require` is
			// called, not inside its argument list: building the message
			// inline would evaluate `front()` on an empty vector on the
			// passing path, which is a crash rather than a pass.
			std::vector<std::string> onlyAuthored;
			for (auto const& [tile, artwork] : authored)
				if (!declared.count(tile)) onlyAuthored.push_back(tile);
			std::string authoredExample;
			if (!onlyAuthored.empty())
				authoredExample = " - starting with '" + onlyAuthored.front() + "'";
			require(onlyAuthored.empty(), name + "'s authored tileset authors "
				+ std::to_string(onlyAuthored.size()) + " tile(s) the manifest's "
				"`ImageSet` never declares" + authoredExample
				+ ". The generated manifest is behind the tileset: regenerate it, "
				"or the tile is artwork nothing will ever draw.");
			std::vector<std::string> onlyDeclared;
			for (auto const& [tile, region] : declared)
				if (!authored.count(tile)) onlyDeclared.push_back(tile);
			std::string declaredExample;
			if (!onlyDeclared.empty())
				declaredExample = " - starting with '" + onlyDeclared.front() + "'";
			require(onlyDeclared.empty(), name + "'s `ImageSet` declares "
				+ std::to_string(onlyDeclared.size()) + " tile(s) its authored "
				"tileset never defines" + declaredExample
				+ "'. The install refuses this outright; regenerate the manifest "
				"from the tileset.");
		}
	}

	// Loads the four viewport atlases into the slots each is named for, without
	// installing anything. A check that wants to drive the install itself -
	// and read the installer's counts afterwards - starts here (#1035).
	pf::render::ViewportAtlasSlots loadViewportAtlasQuadruple(
		StagedAtlasDirectory& staged)
	{
		pf::render::ViewportAtlasSlots slots;
		slots.sectorName = "SectorAtlas";
		slots.sector = staged.load(slots.sectorName);
		slots.objectName = "ObjectAtlas";
		slots.objects = staged.load(slots.objectName);
		slots.agentName = "AgentAtlas";
		slots.agent = staged.load(slots.agentName);
		slots.furnitureName = "FurnitureAtlas";
		slots.furniture = staged.load(slots.furnitureName);
		return slots;
	}

	// Installs the four viewport atlases through the render stack's own pass and
	// hands back the slots so a check can read what landed.
	pf::render::ViewportAtlasSlots installViewportAtlasQuadruple(
		StagedAtlasDirectory& staged)
	{
		auto slots = loadViewportAtlasQuadruple(staged);
		pf::render::AtlasInstaller{}.installAll(slots,
			/*objectsAreMandatory=*/ true);
		return slots;
	}

	// ------------------------------------------------------------------
	// AC (#1026, #1022): the four slots - the Objects slot included - are
	// filled by the one pass the application runs, and what lands in each
	// is the atlas that slot was named for.
	// ------------------------------------------------------------------
	void theFourSlotsInstallThroughTheRenderStacksPass(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		auto const manifest = YAML::LoadFile(
			context.fixture("resources/Resources.yaml").string());
		auto const entries = manifestEntries(manifest);
		auto const registry = atlasSetRegistry(context);
		StagedAtlasDirectory staged{ context, "new-quadruple", kViewportAtlases };
		auto const slots = installViewportAtlasQuadruple(staged);

		require(slots.sectorDiagnostic.empty(), "The Sector slot refused its "
			"shipped atlas: " + slots.sectorDiagnostic);
		require(slots.objectDiagnostic.empty(), "The Objects slot refused its "
			"shipped atlas: " + slots.objectDiagnostic);
		require(slots.agentDiagnostic.empty(), "The Agent slot refused its "
			"shipped atlas: " + slots.agentDiagnostic);
		require(slots.furnitureDiagnostic.empty(), "The Furniture slot refused "
			"its shipped atlas: " + slots.furnitureDiagnostic);

		// The Sector slot: the family came through with the artwork, and every
		// surface the new sheet authors resolves.
		require(sectorTilesetIsNewFamily(drawingSectorSheet()), "The installed Sector atlas did not "
			"come up as the New family, so the Shuttle and Staircase would fall "
			"back to borrowed art (#1027)");
		// Every tile the SectorAtlas manifest declares is installed, and each
		// lands on the side of the renderer's split the authored file put it
		// on. A doorframe is a tile of this atlas but not a Sector surface,
		// and the query that answers "is this a Sector surface" is asked
		// about every name this loop could reach - so a frame that arrived in
		// the surfaces map would answer it wrongly, and the borrow rule and
		// the swatch strip would start treating a frame as a wall (#1054).
		auto const authoredSector = SectorTileset::load(
			context.fixture("resources/textures/sectors-new.tileset.yaml"));
		std::size_t framesInstalled{};
		for (auto const& [tile, region] : entries.at("SectorAtlas").regions)
		{
			if (authoredSector.overlays.count(tile) == 0)
			{
				require(hasSectorTileSurface(drawingSectorSheet(), tile), "The installed Sector atlas "
					"has no '" + tile + "' surface, though SectorAtlas declares "
					"it at " + regionsText(region));
				continue;
			}
			require(hasSectorTileOverlay(drawingSectorSheet(), tile), "The installed Sector atlas has "
				"no '" + tile + "' doorframe, though SectorAtlas declares it at "
				+ regionsText(region));
			require(!hasSectorTileSurface(drawingSectorSheet(), tile), "The installed Sector atlas "
				"answers 'is this a Sector surface' yes for the doorframe tile "
				+ tile + "; a frame is not a surface a Sector is authored with");
			++framesInstalled;
		}
		require(framesInstalled == authoredSector.overlays.size(),
			std::to_string(framesInstalled) + " doorframes were installed from a "
			"manifest that declares none of them, while the authored set holds "
			+ std::to_string(authoredSector.overlays.size()));

		// The Objects slot, read back through the renderer's own surface: the
		// installed artwork per tile is the manifest's region under the
		// authored semantics. This is the slot #1026 left unselected, so it is
		// the one whose wiring matters most here.
		auto const authoredObjects = authoredTilesetTiles(context,
			registry.at("ObjectAtlas"));
		std::size_t objectsReadBack{};
		for (auto const& [tile, region] : entries.at("ObjectAtlas").regions)
		{
			auto const artwork = objectTileArtwork(tile);
			require(artwork.has_value(), "The Objects slot holds no artwork for '"
				+ tile + "', which ObjectAtlas declares, so the Objects slot is "
				"not being filled by the install pass");
			require(artwork->region.x == region.x && artwork->region.y == region.y
				&& artwork->region.width == region.width
				&& artwork->region.height == region.height, "The Objects slot "
				"holds '" + tile + "' at " + regionsText(artwork->region)
				+ " but ObjectAtlas declares " + regionsText(region));
			auto const author = authoredObjects.at(tile);
			require(artwork->offset.x == author.offset.x
				&& artwork->offset.y == author.offset.y
				&& artwork->drawnSize.x == author.drawnSize.x
				&& artwork->drawnSize.y == author.drawnSize.y, "The Objects slot "
				"holds '" + tile + "' with render semantics that are not the "
				"ones objects-new.tileset.yaml authors");
			++objectsReadBack;
		}
		// The read-back covers the whole authored set rather than merely
		// some of it: a slot filled with part of the sheet draws part of the
		// World and magenta for the rest. Counted against the tileset rather
		// than a remembered number, so the bound follows the sheet instead
		// of being left behind when one tile is dropped from it (#1031).
		require(objectsReadBack == authoredObjects.size(),
			std::to_string(objectsReadBack) + " Objects tiles were read back "
			"through the renderer's surface, but objects-new.tileset.yaml authors "
			+ std::to_string(authoredObjects.size()));

		// The Agent and Furniture slots, read back the same way.
		for (auto const& [tile, region] : entries.at("AgentAtlas").regions)
		{
			auto const size = agentSpriteSize(tile.c_str());
			require(size.has_value(), "The Agent slot resolves no '" + tile
				+ "' that AgentAtlas declares");
			require(int(size->x) == region.width && int(size->y) == region.height,
				"The Agent slot resolves '" + tile + "' to " + std::to_string(
					int(size->x)) + "x" + std::to_string(int(size->y))
				+ " but AgentAtlas declares " + regionsText(region));
		}
		for (auto const& [tile, region] : entries.at("FurnitureAtlas").regions)
		{
			auto const size = furnitureSpriteSize(tile.c_str());
			require(size.has_value(), "The Furniture slot resolves no '" + tile
				+ "' that FurnitureAtlas declares");
			require(int(size->x) == region.width && int(size->y) == region.height,
				"The Furniture slot resolves '" + tile + "' to " + std::to_string(
					int(size->x)) + "x" + std::to_string(int(size->y))
				+ " but FurnitureAtlas declares " + regionsText(region));
		}
	}

	// ------------------------------------------------------------------
	// A slot with nothing behind it is reported and left empty, not left
	// holding whatever was installed before it.
	// ------------------------------------------------------------------
	void aMissingAtlasLeavesItsSlotEmptyAndReported(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		StagedAtlasDirectory staged{ context, "empty-slots",
			{ "ObjectAtlas" } };
		pf::render::ViewportAtlasSlots slots;
		slots.objectName = "ObjectAtlas";
		slots.objects = staged.load(slots.objectName);
		// The other three slots name atlases but carry no loaded resource,
		// which is what a failed `requireImageSet` leaves behind.
		slots.sectorName = "SectorAtlas";
		slots.agentName = "AgentAtlas";
		slots.furnitureName = "FurnitureAtlas";
		pf::render::AtlasInstaller{}.installAll(slots,
			/*objectsAreMandatory=*/ false);

		require(slots.sectorDiagnostic.find("not an installed ImageSet")
			!= std::string::npos, "An empty Sector slot reported: '"
			+ slots.sectorDiagnostic + "'");
		require(slots.agentDiagnostic.find("not an installed ImageSet")
			!= std::string::npos, "An empty Agent slot reported: '"
			+ slots.agentDiagnostic + "'");
		require(slots.furnitureDiagnostic.find("not an installed ImageSet")
			!= std::string::npos, "An empty Furniture slot reported: '"
			+ slots.furnitureDiagnostic + "'");
		require(!hasSectorTileSurface(drawingSectorSheet(), "room"), "An empty Sector slot still "
			"resolves a surface, so a failed load would draw stale art");
		require(!agentSpriteSize("human-standing").has_value(), "An empty Agent "
			"slot still resolves a sprite");
		require(!furnitureSpriteSize("chair-L0").has_value(), "An empty Furniture "
			"slot still resolves a sprite");
		// The slot that did load is unaffected by its three empty neighbours.
		require(objectTileArtwork("door-L0").has_value(), "The Objects slot lost its "
			"artwork because a neighbouring slot was empty");
	}

	// ------------------------------------------------------------------
	// A mandatory Objects install refuses outright. The Objects slot is the
	// one whose failure cannot be drawn past, because the object renderer
	// draws Doors, Windows, Lifts and Shuttles at the extents the atlas
	// declares (#1022).
	// ------------------------------------------------------------------
	void aBadObjectsAtlasRefusesAMandatoryInstall(smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		StagedAtlasDirectory staged{ context, "mandatory-objects-drift",
			{ "ObjectAtlas" }, "ObjectAtlas", [](YAML::Node regions)
			{
				// The Door tile taken out of the manifest entirely. The Objects
				// slot draws its Doors, Windows, Lifts and Shuttles at the
				// extents the atlas declares, so a set missing the Door cannot
				// carry the geometry and a mandatory install has to refuse.
				for (std::size_t index = 0; index < regions.size(); ++index)
				{
					if (regions[index]["name"].as<std::string>() != "door-L0") continue;
					regions.remove(index);
					break;
				}
			} };

		pf::render::ViewportAtlasSlots slots;
		slots.objectName = "ObjectAtlas";
		slots.objects = staged.load("ObjectAtlas");
		bool threw{ false };
		std::string message;
		try
		{
			pf::render::AtlasInstaller{}.installAll(slots,
				/*objectsAreMandatory=*/ true);
		}
		catch (std::exception const& error) { threw = true; message = error.what(); }
		require(threw, "A mandatory Objects install accepted an atlas that cannot "
			"carry the object geometry, so start-up would draw a Door's artwork "
			"outside the Door's collision");
		require(message.find("door-L0") != std::string::npos, "The mandatory refusal "
			"does not name the missing Door leaf tile: " + message);
		require(!slots.objectDiagnostic.empty(), "The mandatory refusal left the "
			"Objects diagnostic empty");
		require(!objectTileArtwork("door-L0").has_value(), "The refused Objects "
			"atlas was installed anyway, so the contract reported without "
			"stopping the draw");
	}

	// Runs one registry lookup and returns the thrown refusal's text, or an
	// empty string when the entry resolved.
	std::string registryRefusal(pf::render::AtlasSetRegistry const& registry,
		std::string const& atlas)
	{
		try { registry.require(atlas); return {}; }
		catch (std::exception const& error) { return error.what(); }
	}

	// ------------------------------------------------------------------
	// AC (#1035): the parsed registry answers what the install asks of it,
	// and still refuses what names nothing. The install reads the registry
	// through this type now, so the refusals the per-atlas read used to
	// raise have to survive the move rather than quietly become something
	// else - or nothing.
	// ------------------------------------------------------------------
	void theParsedRegistryAnswersAndRefuses(smoke::Context const& context)
	{
		auto const directory = context.temporaryRoot() / "registry-parse";
		std::filesystem::create_directories(directory);
		auto const path = directory / "atlas-sets.yaml";
		{
			std::ofstream out(path);
			require(bool(out), "Could not write the staged registry into "
				+ directory.string());
			out << "version: 1\n"
				<< "sets:\n"
				<< "  GoodSectorAtlas:\n"
				<< "    kind: sector\n"
				<< "    tileset: textures/sectors-new.tileset.yaml\n"
				<< "  GoodObjectAtlas:\n"
				<< "    kind: object\n"
				<< "    tileset: textures/objects-new.tileset.yaml\n"
				<< "  NoTilesetAtlas:\n"
				<< "    kind: object\n"
				<< "    tileset: \"\"\n"
				<< "  NoKindAtlas:\n"
				<< "    tileset: textures/objects-new.tileset.yaml\n"
				<< "  BogusKindAtlas:\n"
				<< "    kind: mosaic\n"
				<< "    tileset: textures/objects-new.tileset.yaml\n"
				<< "  NotAMapAtlas: 7\n";
		}
		auto const registry = pf::render::AtlasSetRegistry::load(path);
		require(registry.path() == path, "The parsed registry does not name the "
			"file it came from, so its refusals would name somewhere else: "
			+ registry.path().string());
		// Five entries are entries. The one that is not a map names nothing,
		// and throwing over it would take every slot down with it.
		require(registry.size() == 5, "The staged registry parsed "
			+ std::to_string(registry.size()) + " entries; five are maps, and the "
			"scalar one is not an entry at all");

		auto const sector = registry.require("GoodSectorAtlas");
		require(sector.format == pf::render::AtlasAuthoredFormat::SectorTiles,
			"A `kind: sector` entry did not resolve to the sector authored format");
		require(sector.path == directory / "textures/sectors-new.tileset.yaml",
			"The tileset path is not resolved against the registry's own directory: "
			+ sector.path.string());
		require(registry.require("GoodObjectAtlas").format
			== pf::render::AtlasAuthoredFormat::ObjectTiles,
			"A `kind: object` entry did not resolve to the object authored format");
		require(registry.find("NotAMapAtlas") == nullptr, "An entry that is not a "
			"map was parsed as one, so the install would read a scalar as a tileset");
		require(registry.find("NobodyNamedThis") == nullptr, "An atlas the registry "
			"never mentions resolved to something");

		// Every refusal names the atlas, the reason and the file, which is
		// what made the per-atlas read actionable and what the install's
		// diagnostics are built out of.
		auto const noTileset = registryRefusal(registry, "NoTilesetAtlas");
		require(noTileset.find("NoTilesetAtlas") != std::string::npos
			&& noTileset.find("its tileset is empty") != std::string::npos
			&& noTileset.find(path.string()) != std::string::npos,
			"An entry with no tileset refused as: '" + noTileset + "'");
		require(registryRefusal(registry, "NoKindAtlas").find("its kind '' is "
			"neither 'sector' nor 'object'") != std::string::npos,
			"An entry with no kind refused as: '" + registryRefusal(registry,
				"NoKindAtlas") + "'");
		require(registryRefusal(registry, "BogusKindAtlas").find("'mosaic' is "
			"neither 'sector' nor 'object'") != std::string::npos,
			"An entry with an unknown kind refused as: '" + registryRefusal(registry,
				"BogusKindAtlas") + "'");
		require(registryRefusal(registry, "NotAMapAtlas").find("no map entry "
			"names it") != std::string::npos, "An atlas with no map entry refused "
			"as: '" + registryRefusal(registry, "NotAMapAtlas") + "'");

		// A registry that cannot be read, and one that can be read but says
		// nothing, are different refusals and both are said out loud.
		std::string unreadable;
		try { pf::render::AtlasSetRegistry::load(directory / "no-such-file.yaml"); }
		catch (std::exception const& error) { unreadable = error.what(); }
		require(unreadable.find("Could not read the authored atlas-set registry")
			!= std::string::npos, "A missing registry file refused as: '"
			+ unreadable + "'");
		auto const empty = directory / "not-a-registry.yaml";
		{
			std::ofstream out(empty);
			out << "version: 1\n";
		}
		std::string noSets;
		try { pf::render::AtlasSetRegistry::load(empty); }
		catch (std::exception const& error) { noSets = error.what(); }
		require(noSets.find("has no `sets` map") != std::string::npos,
			"A registry with no `sets` map refused as: '" + noSets + "'");
	}

	// ------------------------------------------------------------------
	// AC (#1035): the atlas-set registry is parsed once per stack.
	//
	// The registry is one small file that says which authored tileset each
	// atlas is installed from, and every atlas install asks it that
	// question. Reading it off disk per atlas made one artwork switch cost
	// eight YAML reads, four of them the same registry file over and over.
	// The counts come off the installer's own seam, so what is asserted is
	// what the install did rather than how this file reads.
	// ------------------------------------------------------------------
	void theAtlasSetRegistryIsParsedOncePerStack(smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		StagedAtlasDirectory staged{ context, "registry-reads", kViewportAtlases };
		auto slots = loadViewportAtlasQuadruple(staged);
		pf::render::AtlasInstaller installer;
		installer.installAll(slots, /*objectsAreMandatory=*/ true);
		require(slots.sectorDiagnostic.empty() && slots.objectDiagnostic.empty()
			&& slots.agentDiagnostic.empty() && slots.furnitureDiagnostic.empty(),
			"The four-slot install refused a shipped atlas: "
			+ slots.sectorDiagnostic + " / " + slots.objectDiagnostic + " / "
			+ slots.agentDiagnostic + " / " + slots.furnitureDiagnostic);

		auto const reads = installer.reads();
		require(reads.registryParses == 1, "Installing the four viewport atlases "
			"parsed atlas-sets.yaml " + std::to_string(reads.registryParses)
			+ " time(s). Four atlases asking which tileset they use is four "
			"lookups in one parsed registry, not four reads of the file (#1035).");
		require(reads.tilesetLoads == kViewportAtlases.size(), "The four-slot install "
			"read " + std::to_string(reads.tilesetLoads) + " authored tilesets; "
			"four slots read one each, so anything else means a slot installed "
			"twice or not at all");
		for (auto const& name : kViewportAtlases)
			require(tilesetLoadsFor(reads, name) == 1, name + "'s authored tileset "
				"was read " + std::to_string(tilesetLoadsFor(reads, name))
				+ " time(s) by one four-slot install");

		// A second pass over the same stack - what re-selecting every slot
		// amounts to - still does not go back to the registry. The tilesets
		// are read again because that pass asked for them; the registry is
		// not, because the installer already holds it parsed.
		installer.installAll(slots, /*objectsAreMandatory=*/ true);
		require(installer.reads().registryParses == 1, "A second install pass "
			"over the same installer took the parse count to "
			+ std::to_string(installer.reads().registryParses)
			+ "; the registry is parsed once per stack, not once per pass (#1035).");
		require(installer.reads().tilesetLoads == kViewportAtlases.size() * 2,
			"Two four-slot passes read " + std::to_string(installer.reads().tilesetLoads)
			+ " authored tilesets between them; two passes over four slots read "
			"eight, so one of them is not doing what it says");
	}

	// ------------------------------------------------------------------
	// AC (#1035): selecting one atlas installs one slot. The three atlases
	// that did not change are not read again; only the one that did is.
	//
	// This is the shape every `selectWorld*ImageSet` drives: the new atlas
	// loaded over the slot, then that slot installed. The stack itself needs
	// a live GL context, so what is driven here is the install half - the
	// half that touches disk - which is the half this ticket is about.
	// ------------------------------------------------------------------
	void selectingOneAtlasReReadsOnlyItsOwnTileset(smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		// The four viewport sheets the stack starts on, plus the two themed
		// sheets the selections below switch to. They are different files, so
		// a re-read of a sheet that was not selected cannot hide behind a
		// re-read of the one that was.
		StagedAtlasDirectory staged{ context, "selection-reads",
			{ "SectorAtlas", "ObjectAtlas", "AgentAtlas", "FurnitureAtlas",
				"StationAtlas", "StationFurnitureAtlas" } };
		auto slots = loadViewportAtlasQuadruple(staged);
		pf::render::AtlasInstaller installer;
		installer.installAll(slots, /*objectsAreMandatory=*/ true);
		auto const baseline = installer.reads();
		require(baseline.registryParses == 1 && baseline.tilesetLoads == 4,
			"The starting four-slot pass read "
			+ std::to_string(baseline.registryParses) + " registry file(s) and "
			+ std::to_string(baseline.tilesetLoads) + " tileset(s); the selection "
			"counts below are differences against that, so it has to be the "
			"four-slot pass first");

		// The Agent slot. It is object-shaped and carries no Objects contract
		// of its own, so the themed Furniture sheet is a legal thing to point
		// it at, and it is a different authored file from the one it replaces.
		std::string agentDiagnostic;
		installer.installObjectSlot("StationFurnitureAtlas",
			staged.load("StationFurnitureAtlas"), setAgentTileset,
			agentDiagnostic, /*mandatory=*/ false,
			/*enforceObjectContract=*/ false);
		require(agentDiagnostic.empty(), "Switching the Agent slot to the themed "
			"StationFurnitureAtlas was refused: " + agentDiagnostic);
		auto const afterAgent = installer.reads();
		require(afterAgent.registryParses == baseline.registryParses,
			"Selecting one atlas went back to atlas-sets.yaml: "
			+ std::to_string(baseline.registryParses) + " parse(s) before the "
			"selection, " + std::to_string(afterAgent.registryParses)
			+ " after (#1035).");
		require(afterAgent.tilesetLoads == baseline.tilesetLoads + 1,
			"Selecting the Agent slot read "
			+ std::to_string(afterAgent.tilesetLoads - baseline.tilesetLoads)
			+ " authored tilesets; one slot changed, so only the atlas that "
			"changed may be read");
		require(tilesetLoadsFor(afterAgent, "StationFurnitureAtlas") == 1,
			"The selected StationFurnitureAtlas was read "
			+ std::to_string(tilesetLoadsFor(afterAgent, "StationFurnitureAtlas"))
			+ " time(s) by the selection that selected it");
		// The three slots nobody touched keep what the starting pass installed
		// for them, and were not read again to find out.
		for (auto const* name : { "SectorAtlas", "ObjectAtlas",
			"FurnitureAtlas" })
			require(tilesetLoadsFor(afterAgent, name) == 1, std::string{ name }
				+ "'s authored "
				"tileset was read " + std::to_string(tilesetLoadsFor(afterAgent,
					name)) + " time(s) by a selection that changed the Agent "
				"slot; an unchanged slot keeps the tileset it was installed from "
				"instead of re-reading it (#1035)");
		require(agentSpriteSize("chair").has_value(), "The Agent slot holds no "
			"artwork after a selection the install accepted, so the selection "
			"reported without installing");

		// The Sector selection, which is the other-shaped slot.
		std::string sectorDiagnostic;
		installer.installSectorSlot("StationAtlas", staged.load("StationAtlas"),
			sectorDiagnostic);
		require(sectorDiagnostic.empty(), "Switching the Sector slot to the "
			"themed StationAtlas was refused: " + sectorDiagnostic);
		auto const afterSector = installer.reads();
		require(afterSector.registryParses == 1, "Two selections later the "
			"registry has been parsed " + std::to_string(afterSector.registryParses)
			+ " time(s); one stack parses it once (#1035).");
		require(afterSector.tilesetLoads == baseline.tilesetLoads + 2,
			"Two single-slot selections read "
			+ std::to_string(afterSector.tilesetLoads - baseline.tilesetLoads)
			+ " tilesets beyond the starting pass; two selections read two");
		require(tilesetLoadsFor(afterSector, "StationAtlas") == 1, "The selected "
			"StationAtlas was read " + std::to_string(tilesetLoadsFor(afterSector,
				"StationAtlas")) + " time(s) by the selection that selected it");
		for (auto const* name : { "ObjectAtlas", "AgentAtlas",
			"FurnitureAtlas" })
			require(tilesetLoadsFor(afterSector, name) == 1, std::string{ name }
				+ "'s authored "
				"tileset was read " + std::to_string(tilesetLoadsFor(afterSector,
					name)) + " time(s) by a selection that changed the Sector "
				"slot (#1035)");
		require(hasSectorTileSurface(drawingSectorSheet(), "corridor"), "The Sector slot does not "
			"resolve the themed sheet's `corridor` surface after being switched "
			"to it, so the selection reported without installing");
	}

	// ------------------------------------------------------------------
	// The shipped bundle, paired through the install path.
	// ------------------------------------------------------------------

	std::filesystem::path bundledResourceDirectory(
		smoke::Context const& context)
	{
		return context.fixture("resources/test-worlds/human.agent.lua").parent_path();
	}

	std::string declaredTypeId(std::string const& source,
		std::filesystem::path const& path)
	{
		auto const key = std::string{ "type_id" };
		auto const at = source.find(key);
		require(at != std::string::npos, "The bundled Agent type "
			+ path.filename().string() + " declares no type_id");
		auto const first = source.find('"', at + key.size());
		auto const last = first == std::string::npos
			? std::string::npos : source.find('"', first + 1);
		require(first != std::string::npos && last != std::string::npos,
			"The bundled Agent type " + path.filename().string()
			+ " has an unterminated type_id");
		return source.substr(first + 1, last - first - 1);
	}

	// Attaches one shipped Agent type, or accepts the registration the World
	// already carries - the bundled Human is registered by every World's own
	// constructor from the embed generated from that same resource. Accepting
	// an existing registration is only honest when it names the bundled file,
	// which the second half of this insists on.
	void attachShippedType(core::World& world, std::string const& resourceName,
		std::string const& typeId, std::string const& source)
	{
		if (world.hasAgentType(typeId))
		{
			auto const registered = world.agentTypeResourceName(typeId);
			require(registered == resourceName, "The World already registers the "
				"shipped " + typeId + " Agent type under '" + registered
				+ "' rather than the bundled '" + resourceName + "', so the "
				"pairing would be asserted against a different source");
			return;
		}
		std::string diagnostic;
		bool const attached = world.attachAgentType(resourceName, source,
			&diagnostic);
		require(attached, "Could not attach the shipped " + typeId
			+ " Agent type: " + diagnostic);
	}

	// ------------------------------------------------------------------
	// AC (#1028): every shipped Agent type's declared Pose tiles resolve in
	// the installed viewport Agent atlas. The tiles come off the shipped
	// types' own frozen baselines, and the atlas they resolve against is the
	// one the install path put in the Agent slot - not a tileset loaded
	// beside it.
	// ------------------------------------------------------------------
	void everyShippedAgentPoseTileResolvesInTheInstalledAgentAtlas(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		StagedAtlasDirectory staged{ context, "shipped-agent-tiles", kViewportAtlases };
		installViewportAtlasQuadruple(staged);
		require(agentSpriteSize("android-standing").has_value(), "The installed "
			"Agent atlas resolves no `android-standing`, so the pairing below "
			"would be asserting against an empty slot");

		auto world = std::make_shared<core::World>("Shipped Agent tiles", 8, 2);
		world->addRoom("Room", 0, 0, 0, 8, 1);
		world->finishBuild();
		world->pauseSimulation();

		auto const suffix = std::string{ ".agent.lua" };
		std::size_t types{};
		std::size_t tiles{};
		for (auto const& entry : std::filesystem::directory_iterator(
				bundledResourceDirectory(context)))
		{
			if (!entry.is_regular_file()) continue;
			auto const resourceName = entry.path().filename().string();
			if (resourceName.size() <= suffix.size()
				|| resourceName.compare(resourceName.size() - suffix.size(),
					suffix.size(), suffix) != 0)
				continue;
			auto const source = readFile(entry.path());
			auto const typeId = declaredTypeId(source, entry.path());
			attachShippedType(*world, resourceName, typeId, source);
			auto const baseline = world->registeredAgentTypeBaseline(typeId,
				world->agentTypeResourceName(typeId));
			require(baseline.has_value(), "The shipped " + typeId
				+ " Agent type has no registered physical baseline, so its Pose "
				"tiles cannot be read off it");
			require(!baseline->poses.empty(), "The shipped " + typeId
				+ " Agent type declares no Poses, so it can never be drawn");
			for (auto const& [pose, definition] : baseline->poses)
			{
				auto const who = "The shipped " + typeId + " Agent type's "
					+ std::string{ core::poseName(pose) } + " Pose names '";
				require(!definition.imageTile.empty(), who + "' declares no image "
					"tile, which no atlas can resolve");
				require(agentSpriteSize(definition.imageTile.c_str()).has_value(), who
					+ definition.imageTile + "', which the installed Agent atlas "
					"does not carry, so every Agent of that type draws the "
					"missing-tile magenta. Repoint the Pose onto a tile the "
					"installed atlas authors.");
				++tiles;
			}
			++types;
		}
		require(types >= 4, "Only " + std::to_string(types) + " bundled "
			"`.agent.lua` Agent types were found in "
			+ bundledResourceDirectory(context).string() + "; the bundle ships "
			"Human, Android, Cleaning Bot and Remote operator, so the pairing is "
			"being asserted against less than the whole bundle");
		require(tiles >= 12, "Only " + std::to_string(tiles) + " shipped Pose "
			"tiles were paired against the installed Agent atlas; the four "
			"shipped Agent types declare twelve between them, so the sweep is "
			"stopping short of the bundle");
	}

	// Which shipped Furniture footprints the Furniture sheet holds back, and why.
	// A gap nobody has to justify is where a magenta rectangle goes to stay
	// unnoticed, so the reason is part of the record rather than a bare name.
	// The same record the Furniture role-vocabulary checks read; honouring it
	// here is what keeps this sweep honest about the one piece the sheet
	// cannot serve (#1029).
	std::map<std::string, std::string> uncoveredFurnitureFootprints(
		smoke::Context const& context)
	{
		auto const tileset = atlasSetRegistry(context).at("FurnitureAtlas").tileset;
		auto const root = YAML::LoadFile(context.fixture("resources/" + tileset).string());
		std::map<std::string, std::string> uncovered;
		auto const node = root["uncovered_footprints"];
		if (!node) return uncovered;
		require(node.IsMap(), "`uncovered_footprints` in " + tileset
			+ " must be a map of `<catalogue>:<piece>` to the reason this sheet "
			"cannot carry it");
		for (auto const& entry : node)
		{
			auto const key = entry.first.as<std::string>();
			auto const reason = entry.second.IsScalar()
				? entry.second.as<std::string>() : std::string{};
			require(!reason.empty(), "`uncovered_footprints` records '" + key
				+ "' with no reason; every recorded gap has to say why the sheet "
				"cannot carry the piece");
			uncovered.emplace(key, reason);
		}
		return uncovered;
	}

	// ------------------------------------------------------------------
	// AC (#1029): every shipped Furniture catalogue's tile images resolve
	// in the installed Furniture atlas.
	// ------------------------------------------------------------------
	void everyShippedFurnitureTileResolvesInTheInstalledFurnitureAtlas(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		StagedAtlasDirectory staged{ context, "shipped-furniture-tiles",
			kViewportAtlases };
		installViewportAtlasQuadruple(staged);
		require(furnitureSpriteSize("chair-L0").has_value(), "The installed "
			"Furniture atlas resolves no `chair-L0`, so the pairing below would "
			"be asserting against an empty slot");

		auto const directory = context.fixture(
			"resources/test-worlds/furniture.furniture.lua").parent_path();
		auto const suffix = std::string{ ".furniture.lua" };
		auto const uncovered = uncoveredFurnitureFootprints(context);
		std::size_t catalogues{};
		std::size_t tiles{};
		std::size_t gaps{};
		for (auto const& entry : std::filesystem::directory_iterator(directory))
		{
			if (!entry.is_regular_file()) continue;
			auto const resourceName = entry.path().filename().string();
			if (resourceName.size() <= suffix.size()
				|| resourceName.compare(resourceName.size() - suffix.size(),
					suffix.size(), suffix) != 0)
				continue;
			auto const catalogue = core::FurnitureCatalogue::readFile(entry.path());
			require(catalogue != nullptr, "Could not read the shipped Furniture "
				"catalogue " + resourceName);
			std::size_t inThisOne{};
			for (auto const& [key, definition] : catalogue->definitions())
			{
				auto const footprint = resourceName + ":" + key;
				if (uncovered.count(footprint))
				{
					++gaps;
					continue;
				}
				for (auto const& tile : definition.tiles)
				{
					require(furnitureSpriteSize(tile.image.c_str()).has_value(), "The "
						"shipped catalogue '" + resourceName + "' names tile '"
						+ tile.image + "' at " + std::to_string(tile.x) + ","
						+ std::to_string(tile.y) + " of its '" + key
						+ "' piece, which the installed Furniture atlas does not "
						"carry, so every placed " + key
						+ " draws the missing-tile magenta.");
					++inThisOne;
				}
			}
			require(inThisOne > 0, "The shipped catalogue '" + resourceName
				+ "' names no tile images at all, so it was skipped");
			catalogues += 1;
			tiles += inThisOne;
		}
		require(catalogues >= 7, "Only " + std::to_string(catalogues)
			+ " bundled `.furniture.lua` catalogues were found in "
			+ directory.string() + "; the bundle ships seven, so the pairing is "
			"being asserted against less than the whole bundle");
		require(tiles >= 10, "Only " + std::to_string(tiles) + " shipped "
			"Furniture tile images were paired against the installed atlas");
		// The recorded gaps are a stated limit, not a silent one: a gap that
		// stops being recorded while the sheet still cannot serve it fails the
		// loop above, and a gap recorded for a piece the sheet now covers is a
		// stale claim this catches.
		require(gaps == uncovered.size(), std::to_string(uncovered.size())
			+ " footprint(s) are recorded as uncovered in the Furniture "
			"tileset but only " + std::to_string(gaps)
			+ " of them are shipped pieces, so a record has gone stale");
	}

	// The middle of the part of a sheet a recorded draw samples, in that
	// sheet's own pixels: the recorded uv multiplied back by the sheet's
	// width and height. In pixels rather than as a fraction, so a check can
	// name the region it expected instead of restating the renderer's
	// half-texel inset against a sheet size it does not own.
	std::pair<float, float> sampledCentre(WorldDrawList const& commands,
		pf::render::InstalledSectorSheet const& sheet)
	{
		float left = 1e9f, right = -1e9f, top = 1e9f, bottom = -1e9f;
		int triangles = 0;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (triangle == nullptr
				|| triangle->texture != WorldDrawList::Texture::SectorAtlas)
				continue;
			++triangles;
			for (int corner = 0; corner < 3; ++corner)
			{
				left = std::min(left, triangle->texcoords[corner].x
					* float(sheet.tileset.width));
				right = std::max(right, triangle->texcoords[corner].x
					* float(sheet.tileset.width));
				top = std::min(top, triangle->texcoords[corner].y
					* float(sheet.tileset.height));
				bottom = std::max(bottom, triangle->texcoords[corner].y
					* float(sheet.tileset.height));
			}
		}
		require(triangles == 2, "One World cell drew " + std::to_string(triangles)
			+ " SectorAtlas triangles where the two one framed tile makes, so what "
			"was sampled back is not one tile's");
		return { (left + right) / 2.0f, (top + bottom) / 2.0f };
	}

	// ------------------------------------------------------------------
	// AC (#1087): installed Sector sheets are held keyed by atlas name, on
	// the install side of the renderer, and resolve there with no render
	// context.
	//
	// Two Sector sheets are installed through the same install the
	// application runs, and both stay held afterwards. They are chosen
	// because they disagree about the one fact the renderer reads off a
	// sheet before it draws anything - which artwork family it is - and
	// about where the same tile name sits on the sheet. A registry that kept
	// one installed sheet could answer one of the two names wrongly; only
	// resolving by name gets both right.
	//
	// Nothing below makes a render stack, an OpenGL context or an MPP
	// resource. The install runs against a real resource manager over the
	// shipped manifest, and the registry is read back through the same
	// header the draw path reads it through.
	// ------------------------------------------------------------------
	void installedSectorSheetsAreKeyedByAtlasName(smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		// The default Sector sheet, a version 2 authored set in the New
		// family, and the Station theme, a version 1 set in the Old family.
		// Both ship, both install, and neither is a copy of the other.
		StagedAtlasDirectory staged{ context, "keyed-sector-sheets",
			{ "SectorAtlas", "StationAtlas" } };
		pf::render::AtlasInstaller installer;
		std::string sectorDiagnostic;
		installer.installSectorSlot("SectorAtlas", staged.load("SectorAtlas"),
			sectorDiagnostic);
		require(sectorDiagnostic.empty(), "Installing the shipped SectorAtlas "
			"into the keyed registry was refused: " + sectorDiagnostic);
		std::string stationDiagnostic;
		installer.installSectorSlot("StationAtlas", staged.load("StationAtlas"),
			stationDiagnostic);
		require(stationDiagnostic.empty(), "Installing the shipped StationAtlas "
			"into the keyed registry was refused: " + stationDiagnostic);

		auto& registry = pf::render::sectorSheetRegistry();
		require(registry.size() == 2, "Two Sector sheets were installed and the "
			"registry holds " + std::to_string(registry.size()) + "; the second "
			"install displaced the first instead of keying beside it, which is "
			"what a single installed sheet does (#1087)");
		auto const* sector = registry.find("SectorAtlas");
		auto const* station = registry.find("StationAtlas");
		require(sector != nullptr, "The registry resolves no Sector sheet under "
			"'SectorAtlas' after an install that named it");
		require(station != nullptr, "The registry resolves no Sector sheet under "
			"'StationAtlas' after an install that named it");
		// The identity travels with the sheet: whoever resolves a name is
		// told which sheet it resolved without asking it a second time.
		require(sector->name == "SectorAtlas", "The sheet resolved under "
			"'SectorAtlas' names itself '" + sector->name + "', so the identity a "
			"holder reads back is not the key it was resolved by");
		require(station->name == "StationAtlas", "The sheet resolved under "
			"'StationAtlas' names itself '" + station->name + "'");
		// The two sheets disagree about the family, so one installed sheet
		// could not answer both of these correctly.
		require(sector->tileset.family == SectorTilesetFamily::New, "The sheet "
			"under 'SectorAtlas' is not the New family its version 2 authored "
			"tileset puts it in, so the registry is not holding what was "
			"installed under that name");
		require(station->tileset.family == SectorTilesetFamily::Old, "The sheet "
			"under 'StationAtlas' is not the Old family its version 1 authored "
			"tileset puts it in");
		// And each holds its own pixels: the same tile name sits somewhere
		// else on each sheet, because they are different artwork.
		auto const sectorCorridor = sector->tileset.surfaces.find("corridor");
		auto const stationCorridor = station->tileset.surfaces.find("corridor");
		require(sectorCorridor != sector->tileset.surfaces.end()
			&& stationCorridor != station->tileset.surfaces.end(),
			"'corridor' is missing from one of the two installed Sector sheets, "
			"so the two sheets are not the pair this check needs");
		require(sectorCorridor->second.x != stationCorridor->second.x
			|| sectorCorridor->second.y != stationCorridor->second.y, "The two "
			"installed Sector sheets hold the same 'corridor' region, so nothing "
			"here can tell one sheet's pixels from the other's");
		// A name nothing installed resolves to nothing, rather than to the
		// sheet that happens to be drawn.
		require(registry.find("SectorAtlasLayer2") == nullptr, "The registry "
			"resolved a Sector sheet under a name no install ever used");
	}

	// ------------------------------------------------------------------
	// AC (#1091): a Layer's Sector sheet installs beside the World's sheet
	// without taking the drawn sheet with it.
	//
	// This is the half the Layers table depends on and the half a keyed
	// registry makes easy to get wrong: putting a second sheet in has to
	// make that sheet resolvable, and must not decide what every Layer that
	// binds nothing draws. Both claims are checked against the same two
	// shipped sheets, because the only version worth having is the one that
	// runs the install the application runs.
	// ------------------------------------------------------------------
	void aLayerSheetInstalledAlongsideNeverTakesTheDrawnSheet(smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		StagedAtlasDirectory staged{ context, "alongside-sector-sheet",
			{ "SectorAtlas", "StationAtlas" } };
		pf::render::AtlasInstaller installer;
		auto& registry = pf::render::sectorSheetRegistry();

		std::string worldDiagnostic;
		installer.installSectorSlot("SectorAtlas", staged.load("SectorAtlas"),
			worldDiagnostic);
		require(worldDiagnostic.empty(), "The World's Sector sheet failed to "
			"install: " + worldDiagnostic);

		std::string layerDiagnostic;
		installer.installSectorSheet("StationAtlas", staged.load("StationAtlas"),
			layerDiagnostic);
		require(layerDiagnostic.empty(), "A Layer's Sector sheet was refused: "
			+ layerDiagnostic);

		// The Layer's sheet resolves under the name it was asked for, and
		// resolves to what that install put there. That is the whole of what
		// the per-Layer resolution (#1090) asks of the registry.
		auto const* layer = registry.find("StationAtlas");
		require(layer != nullptr, "A Layer's Sector sheet installed without being "
			"resolvable under its own name, so a Layer bound to it would draw "
			"the World's sheet instead of its own");
		require(layer->name == "StationAtlas"
				&& layer->tileset.family == SectorTilesetFamily::Old,
			"The sheet resolved under 'StationAtlas' is not the sheet the Layer "
			"install put there");
		// And the drawn sheet is still the World's. The Layers table cannot
		// make this claim for itself: a Layer taking a sheet of its own has
		// to leave every Layer that binds nothing drawing what the World
		// chose, which is the one outcome a per-Layer binding must never
		// have (#1091).
		require(registry.drawingName() == "SectorAtlas", "A Layer's Sector sheet "
			"took the drawn sheet with it - the registry is drawing '"
			+ registry.drawingName() + "' - which would repaint every Layer that "
			"binds nothing");
		require(registry.drawing() == registry.find("SectorAtlas"), "The drawn "
			"sheet is not the World's sheet after a Layer sheet went in beside "
			"it, so the World's own selection stopped being what unbound Layers "
			"draw");
	}

	// ------------------------------------------------------------------
	// AC (#1087): the World's selected Sector sheet installs under its own
	// name and is the sheet the draw path uses.
	//
	// The registry holds both sheets the whole time. What moves with each
	// install is which one the draw path reaches, and the family is what
	// makes the two distinguishable from the draw side: the draw path
	// reports the sheet that was installed last while the registry still
	// resolves the other one as itself.
	// ------------------------------------------------------------------
	void theDrawPathDrawsTheSheetInstalledLast(smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		StagedAtlasDirectory staged{ context, "drawn-sector-sheet",
			{ "SectorAtlas", "StationAtlas" } };
		pf::render::AtlasInstaller installer;
		auto& registry = pf::render::sectorSheetRegistry();

		std::string diagnostic;
		installer.installSectorSlot("SectorAtlas", staged.load("SectorAtlas"),
			diagnostic);
		require(diagnostic.empty(), "The starting install of the shipped "
			"SectorAtlas was refused: " + diagnostic);
		require(registry.drawingName() == "SectorAtlas", "The registry is "
			"drawing '" + registry.drawingName() + "' after an install that named "
			"'SectorAtlas'");
		require(registry.drawing() == registry.find("SectorAtlas"), "The sheet "
			"the draw path resolves is not the sheet held under the name it is "
			"drawing, so the draw path is not reading the keyed entry");
		require(sectorTilesetIsNewFamily(drawingSectorSheet()), "The draw path does not report the "
			"New family it just installed, so the installed sheet is not the one "
			"being drawn");

		diagnostic.clear();
		installer.installSectorSlot("StationAtlas", staged.load("StationAtlas"),
			diagnostic);
		require(diagnostic.empty(), "Selecting the shipped StationAtlas was "
			"refused: " + diagnostic);
		require(registry.drawingName() == "StationAtlas", "The draw path still "
			"draws '" + registry.drawingName() + "' after a selection that named "
			"'StationAtlas'");
		require(!sectorTilesetIsNewFamily(drawingSectorSheet()), "The draw path still reports the New "
			"family after the Old-family Station sheet was selected, so the "
			"selection did not move what gets drawn");
		// The sheet that stopped being drawn is still held, and still answers
		// for itself while it is not being drawn.
		auto const* sector = registry.find("SectorAtlas");
		require(sector != nullptr && sector->tileset.family
			== SectorTilesetFamily::New, "The sheet that stopped being drawn "
			"dropped out of the registry or lost its family, so a later resolve of "
			"its name has nothing to resolve to");
		require(registry.size() == 2, "The registry holds "
			+ std::to_string(registry.size()) + " sheets where two were installed");

		// Selecting the first sheet again replaces the entry under its own
		// name and moves the draw path back, with no third sheet behind
		// either key.
		diagnostic.clear();
		installer.installSectorSlot("SectorAtlas", staged.load("SectorAtlas"),
			diagnostic);
		require(diagnostic.empty(), "Selecting the SectorAtlas back was refused: "
			+ diagnostic);
		require(registry.size() == 2, "Selecting a name the registry already "
			"holds grew it to " + std::to_string(registry.size()) + "; a "
			"selection replaces the entry under its own name rather than adding "
			"a second one");
		require(registry.drawingName() == "SectorAtlas", "The draw path draws "
			"'" + registry.drawingName() + "' after the SectorAtlas was selected "
			"back");
		require(sectorTilesetIsNewFamily(drawingSectorSheet()), "The draw path does not report the "
			"New family after the SectorAtlas was selected back");
	}

	// ------------------------------------------------------------------
	// AC (#1088): a Sector draw helper draws the sheet it was handed, not
	// the sheet the install happens to be drawing.
	//
	// Two sheets are installed and one is drawn, exactly as #1087 left it.
	// Pinned here is the other half of the same arrangement: every helper
	// answers off the handle it was given while a different sheet is on the
	// draw path. The family and the variant count answer two different ways
	// of two handles at the one moment - which is the thing a single drawn
	// sheet could never manage - and the pixels a cell samples come from the
	// handed sheet's own region divided by that sheet's own dimensions,
	// which is the one piece of arithmetic a helper cannot get right for
	// two sheets at once if it reads the sheet size from anywhere but its
	// argument.
	// ------------------------------------------------------------------
	void aDrawHelperDrawsTheSheetItIsHanded(smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		StagedAtlasDirectory staged{ context, "handed-sector-sheet",
			{ "SectorAtlas", "StationAtlas" } };
		pf::render::AtlasInstaller installer;
		std::string diagnostic;
		installer.installSectorSlot("SectorAtlas", staged.load("SectorAtlas"),
			diagnostic);
		require(diagnostic.empty(), "Staging the shipped SectorAtlas was "
			"refused: " + diagnostic);
		installer.installSectorSlot("StationAtlas", staged.load("StationAtlas"),
			diagnostic);
		require(diagnostic.empty(), "Staging the shipped StationAtlas was "
			"refused: " + diagnostic);
		// The StationAtlas is what the draw path reaches. Every assertion
		// below is made against the other sheet's handle, so none of them
		// can pass by reading the drawn sheet through the back door.
		auto& registry = pf::render::sectorSheetRegistry();
		require(registry.drawingName() == "StationAtlas", "This check needs the "
			"StationAtlas on the draw path and the SectorAtlas handed in; the "
			"registry is drawing '" + registry.drawingName() + "'");
		auto const* sector = registry.find("SectorAtlas");
		require(sector != nullptr, "The SectorAtlas handle resolved to nothing "
			"after an install that named it");
		require(sector != registry.drawing(), "The handle drawn from here is the "
			"sheet on the draw path, so the two are not distinguishable");
		auto const* drawn = drawingSectorSheet();
		require(drawn != nullptr && drawn->name == "StationAtlas", "The handle "
			"the draw path resolves is not the StationAtlas, so the comparisons "
			"below are not the pair this check is built from");

		// The family answers its argument. Both of these hold at the same
		// moment of two different sheets, which is what a drawn-sheet query
		// could not do.
		require(sectorTilesetIsNewFamily(sector), "The New-family sheet answers "
			"the family question as the Old family when it is handed in as "
			"itself, so the query read the drawn sheet rather than its argument");
		require(!sectorTilesetIsNewFamily(drawn), "The Old-family sheet on the "
			"draw path reports the New family, which is not what it was "
			"installed as");
		// So does the variant count: the New sheet declares three Corridor
		// variants, the Old one declares no families at all.
		require(sectorSurfaceVariantCount(sector, "corridor") == 3,
			"The New-family sheet reports "
			+ std::to_string(sectorSurfaceVariantCount(sector, "corridor"))
			+ " Corridor variants when handed in, so the count came from some "
			"other sheet");
		require(sectorSurfaceVariantCount(drawn, "corridor") == 0,
			"The Old-family sheet reports "
			+ std::to_string(sectorSurfaceVariantCount(drawn, "corridor"))
			+ " Corridor variants, which is more than a set with no declared "
			"families has");

		// The two sheets must disagree about where the same name sits, or the
		// sample read back below says nothing about which sheet it came from.
		auto const& handed = sector->tileset.surfaces.at("corridor");
		auto const& drawnRegion = drawn->tileset.surfaces.at("corridor");
		require(handed.x != drawnRegion.x || handed.y != drawnRegion.y,
			"The two staged sheets hold the same 'corridor' region, so nothing "
			"here can tell one sheet's pixels from the other's");

		// One World cell of `corridor`, drawn from the sheet that is not on
		// the draw path, samples that sheet's own region at that sheet's own
		// dimensions.
		WorldDrawList commands{ WorldDrawList::ClipRectangle{ { 0.0f, 0.0f },
			{ 4096.0f, 4096.0f } } };
		require(drawSectorTileSurface(sector, "corridor", &commands,
				{ 0.0f, 0.0f }, { float(CORE_CELL_WIDTH_PIXELS),
					float(CORE_LEVEL_HEIGHT_PIXELS) }, 1.0f),
			"The per-cell loop refused a tile the sheet it was handed carries");
		auto const centre = sampledCentre(commands, *sector);
		auto const close = [](float a, float b)
		{
			return std::abs(a - b) < 1.0f;
		};
		require(close(centre.first,
				float(handed.x) + float(handed.width) / 2.0f)
				&& close(centre.second,
					float(handed.y) + float(handed.height) / 2.0f),
			"A cell drawn from the SectorAtlas sampled the middle of "
			+ std::to_string(int(centre.first)) + ","
			+ std::to_string(int(centre.second)) + " on that sheet rather than "
			"its own 'corridor' at " + std::to_string(handed.x) + ","
			+ std::to_string(handed.y) + ", so the uv was divided by some other "
			"sheet's width and height");
		// And it is not the drawn sheet's cell: same name, different pixels.
		require(!close(centre.first,
				float(drawnRegion.x) + float(drawnRegion.width) / 2.0f)
				|| !close(centre.second,
					float(drawnRegion.y) + float(drawnRegion.height) / 2.0f),
			"The cell handed the SectorAtlas sampled the drawn sheet's "
			"'corridor' region, so the handle did not decide the pixels");
	}

	// ------------------------------------------------------------------
	// AC (#1087): selecting a Sector sheet the install refuses replaces the
	// entry under its name and reports through the existing diagnostic
	// shape, where empty means accepted.
	//
	// The refused sheet is the shipped SectorAtlas with one manifest tile
	// its authored tileset never defines - the same drift the refusal checks
	// above use - installed over a registry that already holds a good sheet
	// under that name. That is what lets this see whether a refusal left
	// the good artwork standing behind the bad name and reporting nothing.
	// ------------------------------------------------------------------
	void aRefusedSectorSheetInstallReplacesItsEntryAndReports(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		std::string diagnostic;
		// The two staged directories never stand in this process at once.
		// Willpower keeps its resource-definition factories in a
		// process-wide map, so a second live resource manager over a second
		// staged manifest collides with the first the moment it registers.
		// Each stage builds its manager, installs from it, and takes it down
		// before the next one starts; the install copies everything the
		// registry ends up holding, so nothing downstream still needs the
		// manager that produced it.
		{
			StagedAtlasDirectory good{ context, "refused-selection-start",
				{ "SectorAtlas" } };
			pf::render::AtlasInstaller installer;
			installer.installSectorSlot("SectorAtlas", good.load("SectorAtlas"),
				diagnostic);
		}
		require(diagnostic.empty(), "The starting install of the shipped "
			"SectorAtlas was refused: " + diagnostic);
		require(hasSectorTileSurface(drawingSectorSheet(), "corridor"), "The starting install left the "
			"draw path with no Sector surface to draw, so there is nothing here "
			"for a later refusal to replace");

		{
			StagedAtlasDirectory drifted{ context, "refused-selection-drift",
				{ "SectorAtlas" }, "SectorAtlas", [](YAML::Node regions)
				{
					auto added = YAML::Node(YAML::NodeType::Map);
					added["name"] = "tile-nobody-authored";
					added["x"] = "0";
					added["y"] = "0";
					added["width"] = "16";
					added["height"] = "16";
					regions.push_back(added);
				} };
			diagnostic.clear();
			pf::render::AtlasInstaller installer;
			installer.installSectorSlot("SectorAtlas",
				drifted.load("SectorAtlas"), diagnostic);
		}
		require(!diagnostic.empty(), "Selecting a Sector sheet the install "
			"refuses reported an empty diagnostic, so the accepted shape and the "
			"refused shape are not the same shape");
		require(diagnostic.find("tile-nobody-authored") != std::string::npos,
			"The refusal does not name the tile it refused: " + diagnostic);
		auto const* sheet = pf::render::sectorSheetRegistry().find("SectorAtlas");
		require(sheet != nullptr, "The refused selection left no entry at all "
			"under 'SectorAtlas', so the name does not resolve to the sheet that "
			"was refused");
		require(sheet->name == "SectorAtlas", "The entry the refused selection "
			"left names itself '" + sheet->name + "'");
		require(sheet->tileset.surfaces.empty(), "The refused selection left "
			+ std::to_string(sheet->tileset.surfaces.size()) + " surface tile(s) "
			"under 'SectorAtlas', so the artwork the accepted install put there "
			"is still being drawn behind a sheet that was refused");
		require(!hasSectorTileSurface(drawingSectorSheet(), "corridor"), "The draw path still resolves "
			"'corridor' after the selection it refused, so the refusal did not "
			"replace what gets drawn");
	}

	// ------------------------------------------------------------------
	// The two Sector sheets this ticket installs side by side: the World's
	// default and the Layer-1 sheet that ships beside it (#1086). They are
	// the pair every check below needs, because a sheet identity that has
	// only one sheet to choose between is not testable - it cannot be
	// wrong.
	//
	// Both are installed through the install the application runs, and the
	// check then draws from one of them while the draw path is on the other,
	// so no assertion can pass by reading the drawn sheet through the back
	// door.
	// ------------------------------------------------------------------
	class TwoInstalledSheets
	{
	public:
		TwoInstalledSheets(smoke::Context const& context, std::string const& tag)
			: mStaged(context, tag, { "SectorAtlas", "SectorAtlasLayer1" })
		{
			std::string diagnostic;
			mInstaller.installSectorSlot("SectorAtlas",
				mStaged.load("SectorAtlas"), diagnostic);
			require(diagnostic.empty(), "Staging the shipped SectorAtlas beside "
				"the Layer-1 sheet was refused: " + diagnostic);
			diagnostic.clear();
			mInstaller.installSectorSlot("SectorAtlasLayer1",
				mStaged.load("SectorAtlasLayer1"), diagnostic);
			require(diagnostic.empty(), "Staging the shipped SectorAtlasLayer1 "
				"beside the default sheet was refused: " + diagnostic);
			// The Layer-1 sheet was installed last, so it is the sheet the
			// draw path reaches and the default is the one that has to be
			// named from here on. If that ever stops being true the checks
			// below would be comparing a sheet with itself.
			require(pf::render::sectorSheetRegistry().drawingName()
				== "SectorAtlasLayer1", "The Layer-1 sheet was installed last "
				"and the registry is drawing '"
				+ pf::render::sectorSheetRegistry().drawingName()
				+ "', so this fixture is not the pair it claims to be");
			mDefault = pf::render::sectorSheetRegistry().find("SectorAtlas");
			mLayer = pf::render::sectorSheetRegistry().find("SectorAtlasLayer1");
			require(mDefault != nullptr && mLayer != nullptr
				&& mDefault != mLayer, "Two Sector sheets were installed and the "
				"registry does not resolve two distinct entries under their "
				"names, so there is nothing here for an identity to choose "
				"between");
			require(mDefault->installed() && mLayer->installed(), "One of the "
				"two installed Sector sheets has nothing behind its handle, so "
				"the draw below would emit no triangles to read an identity off");
		}

		pf::render::InstalledSectorSheet const* defaultSheet() const
		{
			return mDefault;
		}

		pf::render::InstalledSectorSheet const* layerSheet() const
		{
			return mLayer;
		}

	private:
		StagedAtlasDirectory mStaged;
		pf::render::AtlasInstaller mInstaller;
		pf::render::InstalledSectorSheet const* mDefault{};
		pf::render::InstalledSectorSheet const* mLayer{};
	};

	// The clip every sheet check draws under: wide enough that no cell is
	// culled, so a missing triangle means the sheet did not draw and nothing
	// else.
	WorldDrawList::ClipRectangle sheetCheckClip()
	{
		return { { 0.0f, 0.0f }, { 4096.0f, 4096.0f } };
	}

	// One World cell of `corridor`, drawn from the sheet handed in. Every
	// check that compares two sheets draws this same rect, so the geometry
	// that comes back is the same geometry and the only fact that can tell
	// the two runs apart is the sheet.
	void drawOneCorridorCell(pf::render::InstalledSectorSheet const* sheet,
		WorldDrawList* commands)
	{
		require(drawSectorTileSurface(sheet, "corridor", commands,
				{ 0.0f, 0.0f }, { float(CORE_CELL_WIDTH_PIXELS),
					float(CORE_LEVEL_HEIGHT_PIXELS) }, 1.0f),
			"The per-cell surface loop refused a 'corridor' cell from a sheet "
			"that carries it, so the stream this check reads was never written");
	}

	std::vector<WorldDrawList::Triangle const*> sectorTrianglesOf(
		WorldDrawList const& commands)
	{
		std::vector<WorldDrawList::Triangle const*> triangles;
		for (auto const& command : commands.commands())
			if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(
				&command))
				if (triangle->texture == WorldDrawList::Texture::SectorAtlas)
					triangles.push_back(triangle);
		return triangles;
	}

	// Two triangle runs that differ in nothing but the sheet they carry.
	// Stated as a precondition rather than assumed: if the two draws did not
	// agree on geometry and uv, a split could be blamed on the sheet when
	// it was really caused by something else, and the check would pass
	// without proving anything about the identity.
	bool geometryMatches(WorldDrawList::Triangle const& left,
		WorldDrawList::Triangle const& right)
	{
		for (int corner = 0; corner < 3; ++corner)
			if (left.positions[corner].x != right.positions[corner].x
				|| left.positions[corner].y != right.positions[corner].y
				|| left.texcoords[corner].x != right.texcoords[corner].x
				|| left.texcoords[corner].y != right.texcoords[corner].y)
				return false;
		return left.clip.minimum.x == right.clip.minimum.x
			&& left.clip.minimum.y == right.clip.minimum.y
			&& left.clip.maximum.x == right.clip.maximum.x
			&& left.clip.maximum.y == right.clip.maximum.y;
	}

	// ------------------------------------------------------------------
	// AC (#1089): a recorded Sector triangle keeps the Sector atlas as its
	// family and gains a sheet identity that resolves to an installed sheet
	// with no graphics context.
	//
	// The triangle is recorded by the real per-cell loop handed the real
	// installed sheet and read back through the command seam the renderer
	// exposes. Nothing in this process makes a render stack, an OpenGL
	// context or an MPP resource: the identity resolves through the same
	// GPU-free registry the install wrote (#1087), which is the whole point
	// of putting it on the command rather than on the pass.
	// ------------------------------------------------------------------
	void aSectorTriangleCarriesTheSheetItWasDrawnFrom(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		TwoInstalledSheets sheets{ context, "triangle-sheet-identity" };
		// Drawn from the default sheet while the draw path is on the Layer-1
		// one, so the identity that comes back cannot be the drawn sheet's
		// name picked up off the registry by a helper that ignored its
		// argument.
		WorldDrawList commands{ sheetCheckClip() };
		drawOneCorridorCell(sheets.defaultSheet(), &commands);
		auto const triangles = sectorTrianglesOf(commands);
		require(!triangles.empty(), "Drawing a 'corridor' cell recorded no "
			"Sector-family triangle at all, so there is no recorded command to "
			"read a sheet identity off");

		for (auto const* triangle : triangles)
		{
			// The family is untouched: every existing assertion that a
			// Sector surface triangle's texture is the Sector atlas still
			// reads this field and still means the same thing (#1089).
			require(triangle->texture == WorldDrawList::Texture::SectorAtlas,
				"A Sector surface triangle came back with a texture family other "
				"than the Sector atlas, so the family the existing assertions "
				"read has moved");
			require(triangle->sectorSheet == "SectorAtlas",
				"A triangle drawn from the default Sector sheet carries the "
				"sheet identity '" + triangle->sectorSheet + "', so the "
				"recording did not take the sheet it was handed");
			// The identity resolves here, with no render context: the answer
			// is the sheet the install put under that name, and it is the
			// sheet this triangle was drawn from.
			auto const* resolved = pf::render::sectorSheetRegistry()
				.find(triangle->sectorSheet);
			require(resolved != nullptr, "The sheet identity '"
				+ triangle->sectorSheet + "' resolves to no installed sheet, so "
				"the identity on the command is not a key into what was "
				"installed");
			require(resolved == sheets.defaultSheet(), "The identity on the "
				"triangle resolves to a different sheet than the one it was "
				"drawn from, so resolving it would bind the wrong texture at "
				"batch creation");
			require(resolved->installed(), "The resolved sheet has no pixels "
				"behind its handle, so a batch bound to it would bind nothing");
		}
	}

	// ------------------------------------------------------------------
	// AC (#1089): two installed Sector sheets in one recorded stream
	// become two batches with distinct, resolvable identities, and the
	// sheet is the only thing that could have caused the split.
	//
	// The control is drawn first: one sheet, one rect, one segment. Then
	// the same rect drawn from each of the two sheets into one stream. Same
	// clip, same geometry, same uv - the sheet is the only difference, so
	// a stream that still came back as one segment would be one batch
	// binding whichever texture it was created against, which is the exact
	// failure this ticket exists to remove.
	// ------------------------------------------------------------------
	void twoSheetsInOneStreamBecomeTwoBatches(smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		TwoInstalledSheets sheets{ context, "two-sheets-two-batches" };

		WorldDrawList oneSheet{ sheetCheckClip() };
		drawOneCorridorCell(sheets.defaultSheet(), &oneSheet);
		auto const control = pf::render::buildDrawSegments(oneSheet);
		require(control.size() == 1, "One sheet drawn as one cell came back as "
			+ std::to_string(control.size()) + " segments, so the control is "
			"not one batch and the two-sheet case below proves nothing");
		require(!control[0].triangles.empty(), "The control sheet drew no "
			"triangles into its segment");

		WorldDrawList twoSheets{ sheetCheckClip() };
		drawOneCorridorCell(sheets.defaultSheet(), &twoSheets);
		drawOneCorridorCell(sheets.layerSheet(), &twoSheets);
		auto const segments = pf::render::buildDrawSegments(twoSheets);
		require(segments.size() == 2, "Two Sector sheets drawn into one stream "
			"came back as " + std::to_string(segments.size()) + " segment(s), so "
			"adjacent triangles from different sheets merged into one batch and "
			"one of them will be drawn with the other's texture (#1089)");
		require(segments[0].triangles.size() == control[0].triangles.size()
			&& segments[1].triangles.size() == control[0].triangles.size(),
			"The two-sheet stream did not draw the same number of triangles per "
			"sheet as the control, so the split below is not the sheet's doing");
		for (std::size_t corner = 0; corner < control[0].triangles.size();
			++corner)
			require(geometryMatches(control[0].triangles[corner],
					segments[1].triangles[corner]),
				"The Layer-1 draw produced different geometry or uv from the "
				"default draw, so this stream does not isolate the sheet as the "
				"only reason the two segments are separate");

		// Both keep the Sector family, and each carries its own sheet.
		require(segments[0].texture == WorldDrawList::Texture::SectorAtlas
			&& segments[1].texture == WorldDrawList::Texture::SectorAtlas,
			"A Sector segment lost the Sector family, so the family is no "
			"longer what says which atlas kind a triangle is");
		require(segments[0].sectorSheet == "SectorAtlas"
			&& segments[1].sectorSheet == "SectorAtlasLayer1",
			"The two segments carry '" + segments[0].sectorSheet + "' and '"
			+ segments[1].sectorSheet + "', which are not the two sheets that "
			"were drawn");
		// The identities are distinct and each resolves to the sheet it was
		// drawn from, which is what a batch needs in order to bind the right
		// texture rather than the last one installed.
		auto& registry = pf::render::sectorSheetRegistry();
		require(segments[0].sectorSheet != segments[1].sectorSheet,
			"Two different sheets recorded the same identity, so a batch could "
			"not tell them apart even by resolving it");
		require(registry.find(segments[0].sectorSheet) == sheets.defaultSheet()
			&& registry.find(segments[1].sectorSheet) == sheets.layerSheet(),
			"One of the two recorded identities resolves to a sheet other than "
			"the one its triangles were drawn from");

		// The frame plan puts the two segments in two batches.
		WorldRenderSlotAllocator allocator;
		auto const plan = pf::render::planDrawSlots(segments, allocator);
		require(plan.size() == segments.size(), "The frame plan does not have "
			"one entry per segment, so it cannot be walked in stream order and "
			"draw order would not be preserved");
		require(plan[0] != plan[1], "Both Sector sheets were planned into batch "
			+ std::to_string(plan[0]) + ", which is one batch binding one "
			"texture for two sheets (#1089)");
		require(allocator.size() == 2, "Two sheets in one frame made "
			+ std::to_string(allocator.size()) + " batches; the plan is not "
			"one batch per sheet");
	}

	// ------------------------------------------------------------------
	// AC (#1089): a batch belongs to the sheet it binds, not to where that
	// sheet happened to appear in the stream.
	//
	// Three frames through one allocator: the two sheets in one order, the
	// same two in the other order, then one sheet repeated after the
	// other's segment. The batch each sheet starts with never moves, the
	// reversal makes no new batch, and the repeat takes a further batch of
	// its own sheet rather than the other sheet's. That is what keeps a
	// frame whose visibility changed - a Layer scrolled behind another -
	// from drawing one Layer through the other Layer's batch.
	// ------------------------------------------------------------------
	void eachSheetKeepsItsOwnBatchWhenTheOrderMoves(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		TwoInstalledSheets sheets{ context, "sheet-batch-order" };

		WorldDrawList first{ sheetCheckClip() };
		drawOneCorridorCell(sheets.defaultSheet(), &first);
		drawOneCorridorCell(sheets.layerSheet(), &first);
		WorldDrawList reversed{ sheetCheckClip() };
		drawOneCorridorCell(sheets.layerSheet(), &reversed);
		drawOneCorridorCell(sheets.defaultSheet(), &reversed);
		WorldDrawList interleaved{ sheetCheckClip() };
		drawOneCorridorCell(sheets.defaultSheet(), &interleaved);
		drawOneCorridorCell(sheets.layerSheet(), &interleaved);
		drawOneCorridorCell(sheets.defaultSheet(), &interleaved);

		WorldRenderSlotAllocator allocator;
		auto const forward = pf::render::planDrawSlots(
			pf::render::buildDrawSegments(first), allocator);
		require(forward.size() == 2 && forward[0] != forward[1],
			"The first frame did not put the two sheets in two batches");
		auto const defaultBatch = forward[0];
		auto const layerBatch = forward[1];

		auto const swapped = pf::render::planDrawSlots(
			pf::render::buildDrawSegments(reversed), allocator);
		require(swapped.size() == 2, "The reversed frame planned "
			+ std::to_string(swapped.size()) + " segments where two sheets were "
			"drawn");
		require(swapped[0] == layerBatch, "With the two sheets swapped, the "
			"Layer-1 segment landed in batch " + std::to_string(swapped[0])
			+ " instead of the batch " + std::to_string(layerBatch)
			+ " its own sheet was given, so the batch was keyed by stream "
			"position and would bind the other sheet's texture");
		require(swapped[1] == defaultBatch, "With the two sheets swapped, the "
			"default segment landed in batch " + std::to_string(swapped[1])
			+ " instead of batch " + std::to_string(defaultBatch));

		auto const third = pf::render::planDrawSlots(
			pf::render::buildDrawSegments(interleaved), allocator);
		require(third.size() == 3, "The interleaved frame planned "
			+ std::to_string(third.size()) + " segments where three cells were "
			"drawn");
		require(third[0] == defaultBatch && third[1] == layerBatch,
			"Adding a segment after the two sheets moved either sheet off the "
			"batch its own sheet was given, so a sheet's batch depended on what "
			"came after it in the stream");
		require(third[2] != defaultBatch && third[2] != layerBatch,
			"The repeated default segment was planned into a batch already bound "
			"to another sheet, which would draw it with the wrong texture; a "
			"repeat of one sheet later in the same frame takes a further batch of "
			"its own sheet");
		require(allocator.size() == 3, "Three frames of two sheets made "
			+ std::to_string(allocator.size()) + " batches; only the repeated "
			"sheet's second batch should have been added, and the reversal should "
			"have added none");
	}
	// ------------------------------------------------------------------
	// #1090: a Sector renders from its own Layer's Image set.
	//
	// #1087 put the sheets in a keyed, GPU-free registry, #1088 made every
	// draw helper take the handle it was handed, and #1089 put the sheet on
	// the recorded triangle. What was missing was the draw path asking which
	// sheet a Sector wants. It asks the Sector's own Layer, once per Sector,
	// and hands the handle down - which is why the aperture recursion needs
	// no arithmetic of its own: a back Sector answers for itself the same
	// way a front one does, no matter which pass reached it.
	//
	// Everything below drives the real render pass over a real World with
	// the real install behind it and reads the recorded command stream back.
	// No render stack and no OpenGL context anywhere in the process: the
	// sheet a triangle carries resolves through the install registry
	// (#1087), which is the seam this feature was built to be assertable at.
	// ------------------------------------------------------------------

	// Sector sheets installed through the install the application runs, for
	// the Layer Image set checks. Three are staged because the precedence
	// checks need three distinct answers in play at once: the World's own
	// sheet, the sheet a Layer binds, and a third sheet the World's pick can
	// move to while that Layer keeps its own.
	class InstalledSectorSheets
	{
	public:
		explicit InstalledSectorSheets(smoke::Context const& context,
			std::string const& tag)
			: mStaged(context, tag, { "SectorAtlas", "SectorAtlasLayer1",
				"StationAtlas" })
		{
			// The Layer-1 sheet first and the World's default last, so the
			// sheet the draw path reaches is the World's own and a Layer's
			// binding is the only thing that can move a Layer off it.
			install("SectorAtlasLayer1");
			install("SectorAtlas");
		}

		// Installs one shipped sheet and makes it the sheet the draw path
		// draws, which is what selecting another sector Image set for the
		// World does.
		void install(std::string const& name)
		{
			std::string diagnostic;
			mInstaller.installSectorSlot(name, mStaged.load(name), diagnostic);
			require(diagnostic.empty(), "Staging the shipped " + name
				+ " Sector sheet was refused: " + diagnostic);
			require(pf::render::sectorSheetRegistry().drawingName() == name,
				"Installing " + name + " left the draw path on '"
					+ pf::render::sectorSheetRegistry().drawingName()
					+ "', so the install did not make it the sheet that draws");
		}

		pf::render::InstalledSectorSheet const* find(std::string const& name) const
		{
			return pf::render::sectorSheetRegistry().find(name);
		}

		std::string drawnName() const
		{
			return pf::render::sectorSheetRegistry().drawingName();
		}

	private:
		StagedAtlasDirectory mStaged;
		pf::render::AtlasInstaller mInstaller;
	};

	// A two-Layer World whose front wall carries one Window and one Door,
	// both looking into the Layer behind. Those are the two recursions the
	// binding has to survive: in each case the back Sector is drawn by the
	// front Sector's pass, under a clip the threshold pushed, and must still
	// wear its own Layer's sheet rather than the front Layer's.
	struct LayerSheetScene
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<const core::Sector> front;
		std::shared_ptr<const core::Sector> back;
		std::shared_ptr<core::Door> door;
		std::shared_ptr<const core::Window> window;
	};

	std::shared_ptr<const core::Sector> onlySectorOnLayer(core::World const& world,
		uint32_t layer)
	{
		std::shared_ptr<const core::Sector> found;
		int count{};
		for (uint32_t index = 0; index < world.getNumSectors(); ++index)
		{
			auto const sector = world.getSector(index);
			if (sector && sector->getLayerIndex() == layer)
			{
				found = sector;
				++count;
			}
		}
		require(count == 1, "Layer " + std::to_string(layer) + " holds "
			+ std::to_string(count) + " Sectors; the fixture puts exactly one "
			"wall on each Layer so a recorded pass over it has one answer");
		return found;
	}

	// The threshold authored *on* a Sector, as opposed to the one the Sector
	// only carries as the back registration of a threshold written on the
	// Layer in front of it. Which side a threshold was authored on is the
	// threshold's fact to state, so it is asked rather than guessed.
	std::shared_ptr<core::Door> doorAuthoredOn(
		std::shared_ptr<const core::Sector> const& sector)
	{
		for (uint32_t index = 0; index < sector->getNumObjects(); ++index)
		{
			auto const object = sector->getObject(index);
			if (!object || object->getObjectType() != core::SectorObjectType::Door)
				continue;
			auto const door = std::static_pointer_cast<core::DoorSectorObject>(
				object)->getDoor();
			if (door && door->getFrontSector() == sector) return door;
		}
		return nullptr;
	}

	std::shared_ptr<const core::Window> windowAuthoredOn(
		std::shared_ptr<const core::Sector> const& sector)
	{
		for (uint32_t index = 0; index < sector->getNumObjects(); ++index)
		{
			auto const object = sector->getObject(index);
			if (!object || object->getObjectType()
					!= core::SectorObjectType::Window) continue;
			auto const window = std::static_pointer_cast<
				core::WindowSectorObject>(object)->getWindow();
			if (window && window->getFrontSector() == sector) return window;
		}
		return nullptr;
	}

	// Builds the fixture. `wallKind` says what each Layer carries: `room` is
	// the ordinary case, and `corridor` is what the precedence check needs,
	// because the third sheet the World's pick can move to - the shipped
	// Station theme - is an `ImageSet` that declares one region only,
	// `corridor`. A Room drawn from that sheet answers the missing-tile
	// magenta, which is no Sector-family triangle at all, and there would
	// be nothing left to read a sheet off.
	LayerSheetScene layerSheetScene(std::string const& wallKind = "room")
	{
		auto world = std::make_shared<core::World>("Layer image set world", 16,
			6);
		while (world->getLayerCount() < 2) world->addLayer();
		if (wallKind == "corridor")
		{
			world->addCorridor(0, 0, 0, 4, 1);
			world->addCorridor(1, 0, 0, 4, 1);
		}
		else
		{
			require(wallKind == "room", "The fixture was asked for a wall kind "
				"of '" + wallKind + "', which it knows how to build as neither");
			world->addRoom("Front", 0, 0, 0, 4, 1);
			world->addRoom("Back", 1, 0, 0, 4, 1);
		}
		// One Window at column 1 and one Door at column 3 of the front wall,
		// each with the back wall directly behind it.
		world->addSectorWindow(0, 0, 1, 1, 1);
		world->addSectorDoor(0, 0, 3);
		world->finishBuild();
		world->pauseSimulation();

		LayerSheetScene scene;
		scene.world = world;
		scene.front = onlySectorOnLayer(*world, 0);
		scene.back = onlySectorOnLayer(*world, 1);
		require(scene.front != nullptr && scene.back != nullptr,
			"The Layer Image set fixture lost a Layer's wall, so there is no "
			"front pass to record and nothing behind it to see through an "
			"aperture");
		scene.window = windowAuthoredOn(scene.front);
		scene.door = doorAuthoredOn(scene.front);
		require(scene.window != nullptr && scene.door != nullptr,
			"The fixture's front wall lost the Window or the Door, so one of the "
			"two aperture recursions this feature has to survive is not being "
			"exercised");
		require(scene.window->getBackSector() == scene.back
				&& scene.door->getBackSector() == scene.back,
			"Neither threshold looks into the Layer the fixture put behind it, so "
			"the recursion would draw some other Sector and the sheet read back "
			"would not be Layer 1's");
		return scene;
	}

	// Drives a Door to an exact open fraction through its own timing, so the
	// renderer reads a real device state. A closed Door culls the space
	// behind it - its leaves cover the whole doorway - so a check that wants
	// to see through one has to open it first.
	void driveTo(std::shared_ptr<core::Door> const& door, float fraction)
	{
		door->close();
		while (!door->isClosed()) door->update(door->getOpenCloseTime());
		if (fraction <= 0.0f) return;
		door->open();
		door->update(door->getOpenCloseTime() * fraction);
		require(door->getOpenPercentage() >= fraction,
			"The Door fixture could not be driven to " + std::to_string(fraction)
				+ " open, reached " + std::to_string(door->getOpenPercentage()));
	}

	// The clip every pass here is recorded under: wide enough that nothing is
	// culled, so a missing draw means the pass did not draw and nothing else.
	// Anything drawn under a *narrower* clip came down a threshold's aperture
	// rather than out of the Sector's own pass, which is the one distinction
	// that separates the two Layers in a recorded frame.
	WorldDrawList::ClipRectangle wideClip()
	{
		return { { -100000.0f, -100000.0f }, { 100000.0f, 100000.0f } };
	}

	// Records one Sector through the real pass with the World published, the
	// way renderWorld() publishes it. The scope is what lets the pass read
	// the Layer bindings off the World being drawn.
	WorldDrawList recordSectorPass(std::shared_ptr<const core::World> const& world,
		std::shared_ptr<const core::Sector> const& sector, uint32_t layer,
		LayerRenderStyle style)
	{
		WorldDrawList commands{ wideClip() };
		RenderWorldScope scope(world);
		renderSector(sector, layer, style, false, packColour(214, 96, 32, 255),
			&commands);
		return commands;
	}

	// One Sector-family triangle read back out of a recorded frame, with the
	// two facts every check below sorts on: which sheet it says it came from,
	// and whether it was drawn under the recorder's own clip or under one a
	// threshold pushed.
	struct DrawnSectorTile
	{
		std::string sheet;
		bool ownClip{ true };
	};

	bool sameClip(WorldDrawList::ClipRectangle const& left,
		WorldDrawList::ClipRectangle const& right)
	{
		return left.minimum.x == right.minimum.x
			&& left.minimum.y == right.minimum.y
			&& left.maximum.x == right.maximum.x
			&& left.maximum.y == right.maximum.y;
	}

	std::vector<DrawnSectorTile> drawnSectorTiles(WorldDrawList const& commands)
	{
		auto const outer = wideClip();
		std::vector<DrawnSectorTile> out;
		for (auto const& command : commands.commands())
		{
			if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(
				&command))
			{
				if (triangle->texture == WorldDrawList::Texture::SectorAtlas)
					out.push_back({ triangle->sectorSheet,
						sameClip(triangle->clip, outer) });
			}
		}
		return out;
	}

	std::vector<DrawnSectorTile> underClip(
		std::vector<DrawnSectorTile> const& tiles, bool ownClip)
	{
		std::vector<DrawnSectorTile> out;
		for (auto const& tile : tiles)
			if (tile.ownClip == ownClip) out.push_back(tile);
		return out;
	}

	std::string sheetText(std::vector<DrawnSectorTile> const& tiles)
	{
		std::string out;
		for (auto const& tile : tiles)
		{
			if (!out.empty()) out += ", ";
			out += tile.sheet.empty() ? std::string("<none>") : tile.sheet;
		}
		return out.empty() ? std::string("<nothing>") : out;
	}

	// The distinct sheets a set of tiles carries, in first-drawn order.
	std::vector<std::string> distinctSheets(
		std::vector<DrawnSectorTile> const& tiles)
	{
		std::vector<std::string> out;
		for (auto const& tile : tiles)
			if (std::find(out.begin(), out.end(), tile.sheet) == out.end())
				out.push_back(tile.sheet);
		return out;
	}

	// The one sheet a Sector's own surface drew, asserted to be exactly one.
	// A Sector resolves its sheet once per Sector, so more than one answer
	// across its own cells is the pass disagreeing with itself. `step` names
	// which arrangement of bindings and World pick the read was taken under,
	// because the precedence check below takes seven of them and a failure
	// that does not say which is a failure that has to be found by hand.
	std::string ownSurfaceSheet(std::string const& step,
		std::shared_ptr<const core::World> const& world,
		std::shared_ptr<const core::Sector> const& sector, uint32_t layer)
	{
		auto const tiles = underClip(drawnSectorTiles(recordSectorPass(world,
			sector, layer, LayerRenderStyle::Solid)), true);
		require(!tiles.empty(), step + ": the Layer-" + std::to_string(layer)
			+ " Sector drew no Sector-family tile under its own clip, so there "
			"is no sheet to read back");
		auto const distinct = distinctSheets(tiles);
		require(distinct.size() == 1, step + ": one Sector's own surface drew "
			+ std::to_string(distinct.size()) + " different sheets ('"
			+ sheetText(tiles) + "'); the binding is resolved once per Sector, so "
			"every cell of one Sector draws off one sheet");
		return distinct.front();
	}

	// ------------------------------------------------------------------
	// AC: a Layer with a binding draws its own sheet and a Layer without one
	// draws the World's, in the same frame, with the back Layer reached
	// through an aperture.
	//
	// The two Layers are told apart by their clip as well as by their sheet:
	// the front wall's own tiles are drawn under the clip the recorder opened
	// and the back Sector's under one the Window pushed, so "these are the
	// two Layers" is a statement about where each draw came from rather than
	// only about which string it carries.
	// ------------------------------------------------------------------
	void aBoundLayerAndAnInheritingLayerDrawDifferentSheetsInOneFrame(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		InstalledSectorSheets sheets{ context, "layer-sheet-one-frame" };
		auto const scene = layerSheetScene();
		require(scene.world->layerSectorImageSet(0) == std::nullopt
				&& scene.world->layerSectorImageSet(1) == std::nullopt,
			"A fresh World did not start with every Layer inheriting, so the "
			"baseline below is not the unbound state");
		require(sheets.drawnName() == "SectorAtlas", "This check needs the "
			"World's own sheet on the draw path, and the draw path is on '"
			+ sheets.drawnName() + "'");

		scene.world->setLayerSectorImageSet(1, "SectorAtlasLayer1");

		auto const tiles = drawnSectorTiles(recordSectorPass(scene.world,
			scene.front, 0, LayerRenderStyle::Solid));
		auto const own = underClip(tiles, true);
		auto const through = underClip(tiles, false);
		require(!own.empty(), "The front Sector drew no Sector-family tile at "
			"all, so there is nothing in this frame to read a sheet off");
		require(!through.empty(), "Nothing was drawn through an aperture, so this "
			"frame holds one Layer and cannot show two wearing two sheets");

		for (auto const& tile : own)
			require(tile.sheet == "SectorAtlas", "A tile of the front Layer - "
				"which binds nothing - carries '" + tile.sheet + "'; an "
				"inheriting Layer draws the World's sector Image set, which is the "
				"sheet the install is drawing");
		for (auto const& tile : through)
			require(tile.sheet == "SectorAtlasLayer1", "A Sector reached through "
				"an aperture carries '" + tile.sheet + "'; it is on Layer 1, "
				"which binds SectorAtlasLayer1, and the recursion carries no "
				"binding arithmetic that could have made it wear anything else");

		// Two identities, and they are two installed sheets rather than one
		// name spelled two ways.
		require(distinctSheets(tiles).size() == 2, "One frame carried "
			+ std::to_string(distinctSheets(tiles).size()) + " distinct Sector "
			"sheet identities where two Layers wearing two sheets was asked for: "
			+ sheetText(tiles));
		require(sheets.find("SectorAtlas") != nullptr
				&& sheets.find("SectorAtlasLayer1") != nullptr
				&& sheets.find("SectorAtlas") != sheets.find("SectorAtlasLayer1"),
			"The two identities in the frame do not resolve to two distinct "
			"installed sheets, so a batch bound to one of them would not know "
			"which pixels it was binding");
	}

	// ------------------------------------------------------------------
	// AC: a back Sector seen through a Window or a Door wears its own
	// Layer's sheet, never the front Layer's.
	//
	// The front Layer is the one bound here, which is what makes this the
	// hard direction: every tile of the front wall and of the frames set
	// into it carries the front Layer's own sheet, and the back Sector -
	// drawn by that same pass, from inside the threshold draw - has to come
	// back on the World's sheet rather than picking up the front Layer's.
	// A pass that resolved the sheet once per pass rather than once per
	// Sector could not produce this picture, and a frame pass that still
	// asked the install rather than the threshold's own Sector would come
	// back with the World's sheet on the front wall.
	// ------------------------------------------------------------------
	void aBackSectorThroughAnApertureNeverWearsTheFrontLayersSheet(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		InstalledSectorSheets sheets{ context, "layer-sheet-aperture" };
		auto const scene = layerSheetScene();
		scene.world->setLayerSectorImageSet(0, "SectorAtlasLayer1");
		require(scene.world->layerSectorImageSet(1) == std::nullopt,
			"Layer 1 was expected to be left unbound for this check");

		// The Door is driven open. A closed Door culls the space behind it,
		// so a closed fixture would silently exercise the Window alone and the
		// Door half of the AC would go untested.
		driveTo(scene.door, 0.5f);

		auto const tiles = drawnSectorTiles(recordSectorPass(scene.world,
			scene.front, 0, LayerRenderStyle::Solid));
		auto const own = underClip(tiles, true);
		auto const through = underClip(tiles, false);
		require(own.size() >= 8, "The front wall drew only "
			+ std::to_string(own.size()) + " Sector tiles; a four-cell wall plus "
			"the Window and Door frames over it is more than this, so the frame "
			"passes may not have run");
		require(through.size() >= 4, "Only " + std::to_string(through.size())
			+ " tiles came down an aperture; the back wall behind an open Window "
			"and an open Door is more than one quad, so the recursion may not "
			"have run at all");

		for (auto const& tile : own)
			require(tile.sheet == "SectorAtlasLayer1", "A tile of the bound front "
				"Layer carries '" + tile.sheet + "'; Layer 0 binds "
				"SectorAtlasLayer1, and that includes the frames set into that "
				"wall, which are Sector artwork too");
		for (auto const& tile : through)
			require(tile.sheet == "SectorAtlas", "A Sector reached through an "
				"aperture carries '" + tile.sheet + "'; it is on Layer 1, which "
				"binds nothing and follows the World's pick - the front Layer's "
				"sheet must not bleed into what is seen through an opening");
		require(distinctSheets(tiles).size() == 2, "The front Layer's sheet and "
			"the back Layer's sheet are not two identities in one frame: "
			+ sheetText(tiles));
		// The through draw really is clipped by the threshold rather than by
		// the recorder, which is the only thing that says it came down the
		// recursion instead of being a second pass over the front wall.
		for (auto const& tile : through)
			require(!tile.ownClip, "A back-Layer tile was drawn under the "
				"recorder's own clip, so it did not come down a threshold "
				"aperture and this frame proves nothing about the recursion");
		require(sheets.find("SectorAtlas") != sheets.find("SectorAtlasLayer1"),
			"The two sheets installed for this check are one sheet");
	}

	// ------------------------------------------------------------------
	// AC: override beats the World pick and inherit follows it, asserted
	// directly rather than inferred from one arrangement.
	//
	// Each step moves exactly one thing - a binding, or the World's pick -
	// and both Layers are read after every move, so a rule that followed the
	// install order rather than the binding cannot hide in any one of them.
	// ------------------------------------------------------------------
	void overrideBeatsTheWorldPickAndInheritFollowsIt(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		InstalledSectorSheets sheets{ context, "layer-sheet-precedence" };
		auto const scene = layerSheetScene("corridor");
		require(sheets.drawnName() == "SectorAtlas", "This check starts from the "
			"World picking its own sheet, and the draw path is on '"
			+ sheets.drawnName() + "'");

		// Nothing bound: both Layers follow the World.
		require(ownSurfaceSheet("both Layers inheriting", scene.world,
			scene.front, 0) == "SectorAtlas",
			"An unbound Layer 0 did not draw the World's sector Image set");
		require(ownSurfaceSheet("both Layers inheriting", scene.world,
			scene.back, 1) == "SectorAtlas",
			"An unbound Layer 1 did not draw the World's sector Image set");

		// Bind one Layer: it moves off the World's pick and its neighbour,
		// which still inherits, does not move with it.
		scene.world->setLayerSectorImageSet(1, "SectorAtlasLayer1");
		require(ownSurfaceSheet("Layer 1 bound at SectorAtlasLayer1", scene.world,
			scene.back, 1) == "SectorAtlasLayer1",
			"A bound Layer did not draw its own sheet");
		require(ownSurfaceSheet("Layer 1 bound at SectorAtlasLayer1", scene.world,
			scene.front, 0) == "SectorAtlas",
			"Binding one Layer moved the sheet of a Layer that binds nothing, so "
			"the binding is not per Layer");

		// Move the World's pick. The inheriting Layer follows it; the
		// overriding Layer does not.
		sheets.install("StationAtlas");
		require(ownSurfaceSheet("the World picking StationAtlas", scene.world,
			scene.front, 0) == "StationAtlas",
			"Inherit does not follow the World pick: after the World moved to "
			"StationAtlas an unbound Layer still drew otherwise");
		require(ownSurfaceSheet("the World picking StationAtlas", scene.world,
			scene.back, 1) == "SectorAtlasLayer1",
			"Override does not beat the World pick: a Layer bound to "
			"SectorAtlasLayer1 moved off it when the World picked StationAtlas");

		// Clearing the binding puts the Layer back under the World's pick.
		scene.world->setLayerSectorImageSet(1, std::nullopt);
		require(ownSurfaceSheet("the binding cleared back to inherit", scene.world,
			scene.back, 1) == "StationAtlas",
			"A cleared binding did not return the Layer to the World's pick, so "
			"the clear is not the inherit state");

		// And an override is the sheet named, not the sheet installed first
		// or last: binding the Layer at the World's own sheet draws that
		// sheet, and binding it at a third sheet moves it off the World's
		// pick again while the World has not moved at all.
		scene.world->setLayerSectorImageSet(1, "StationAtlas");
		require(ownSurfaceSheet("Layer 1 bound at the World's own pick", scene.world,
			scene.back, 1) == "StationAtlas",
			"A Layer bound at the sheet the World itself picks did not draw it");
		scene.world->setLayerSectorImageSet(1, "SectorAtlas");
		require(ownSurfaceSheet("Layer 1 bound at a third sheet", scene.world,
			scene.back, 1) == "SectorAtlas",
			"A Layer bound at a sheet other than the World's pick did not follow "
			"its own binding, so the answer is coming from somewhere other than "
			"the Layer's Layer Image set");
		require(sheets.drawnName() == "StationAtlas", "The World's pick moved "
			"while only a binding was being changed, so the two are not the "
			"independent facts the feature says they are");
	}

	// ------------------------------------------------------------------
	// AC: a binding naming a sheet nothing installed draws the World's sheet.
	//
	// The same recoverable answer a recorded triangle naming an unresolvable
	// sheet gets where its batch binds its texture (#1089). A bad artwork
	// choice is reported where it is made and drawn past; it is not a reason
	// for a Layer to draw nothing, and it is not a reason to throw through a
	// render pass that has other Layers to draw.
	// ------------------------------------------------------------------
	void aBindingThatNamesNoInstalledSheetDrawsTheWorldsSheet(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		InstalledSectorSheets sheets{ context, "layer-sheet-unresolvable" };
		auto const scene = layerSheetScene();
		require(sheets.find("SectorAtlasLayer2") == nullptr, "The name this "
			"check calls unresolvable is installed after all, so the fallback "
			"below would not be testing a missing sheet");

		scene.world->setLayerSectorImageSet(1, "SectorAtlasLayer2");
		auto const drawn = ownSurfaceSheet("Layer 1 bound at a name nothing "
			"installed", scene.world, scene.back, 1);
		require(drawn == "SectorAtlas", "A Layer bound at a sheet nothing "
			"installed drew '" + drawn + "'; it should fall back to the World's "
			"sheet, which is what every Sector drew before a Layer could bind "
			"one, rather than drawing nothing or drawing the name it could not "
			"resolve");
		require(sheets.find(drawn) == pf::render::sectorSheetRegistry().drawing(),
			"The fallback sheet is not the sheet the install is drawing, so the "
			"unresolvable binding landed on a third thing");
	}

	// ------------------------------------------------------------------
	// AC: the next-Layer wireframe overlay draws no tiles and is unaffected.
	//
	// The overlay is a drafting aid, not a second artwork surface: it
	// outlines the Layer behind and fills nothing. A binding on the Layer it
	// outlines therefore has nothing to change, and the check says so by
	// finding no Sector-family triangle at all - while still finding the
	// outlines, so an empty pass cannot be read as a successful exclusion.
	// ------------------------------------------------------------------
	void theWireframeOverlayDrawsNoTiles(smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		InstalledSectorSheets sheets{ context, "layer-sheet-wireframe" };
		auto const scene = layerSheetScene();
		scene.world->setLayerSectorImageSet(0, "SectorAtlasLayer1");
		scene.world->setLayerSectorImageSet(1, "SectorAtlasLayer1");

		for (uint32_t layer = 0; layer < 2; ++layer)
		{
			auto const tiles = drawnSectorTiles(recordSectorPass(scene.world,
				layer == 0 ? scene.front : scene.back, layer,
				LayerRenderStyle::Wireframe));
			require(tiles.empty(), "The wireframe pass drew "
				+ std::to_string(tiles.size()) + " Sector-family tile(s) on Layer "
				+ std::to_string(layer) + ": " + sheetText(tiles) + "; the "
				"overlay contributes the outline only and never a fill (ADR 0002), "
				"so a Layer Image set has no tile for it to draw");
		}

		// The overlay still outlined. Without this the check above would pass
		// on a pass that drew nothing at all.
		WorldDrawList overlay{ wideClip() };
		{
			RenderWorldScope scope(scene.world);
			renderSectors(scene.world, 1, LayerRenderStyle::Wireframe, &overlay);
		}
		std::size_t lines{};
		for (auto const& command : overlay.commands())
			if (std::holds_alternative<WorldDrawList::Line>(command)) ++lines;
		require(lines > 0, "The wireframe overlay emitted no lines at all, so "
			"'it drew no tiles' would be a statement about an empty pass");
		require(drawnSectorTiles(overlay).empty(), "The whole-Layer wireframe "
			"overlay drew Sector-family tiles over a bound Layer");
	}

	// ------------------------------------------------------------------
	// AC: the LocationPlan canvas keeps the World's sector sheet.
	//
	// The plan is a World-level tool: it draws a Location's furniture grid
	// from the Furniture atlas and never a Sector surface, so a Layer binding
	// has no route into it. Asserted as the absence of any Sector-family
	// triangle in the plan's own stream, with the plan still having drawn
	// its grid so the absence is the rule and not an empty render.
	// ------------------------------------------------------------------
	void theLocationPlanCanvasKeepsTheWorldsSectorSheet(
		smoke::Context const& context)
	{
		ClearInstalledTilesets guard;
		InstalledSectorSheets sheets{ context, "location-plan-sheet" };
		auto const scene = layerSheetScene();
		// Both Layers bound, so a Sector sheet reaching the plan could only
		// have come from a Layer binding rather than from the World.
		scene.world->setLayerSectorImageSet(0, "SectorAtlasLayer1");
		scene.world->setLayerSectorImageSet(1, "SectorAtlasLayer1");

		WorldDrawList plan{ wideClip() };
		{
			RenderWorldScope scope(scene.world);
			renderLocationPlanGrid(plan, *scene.front, { 0.0f, 0.0f },
				{ 800.0f, 400.0f }, 5, scene.world.get(), 0);
		}
		require(plan.commands().size() > 10, "The LocationPlan grid drew "
			+ std::to_string(plan.commands().size()) + " commands; it draws a "
			"frame, a background and a row of depth labels, so nothing drawing "
			"at all would make the check below vacuous");
		auto const tiles = drawnSectorTiles(plan);
		require(tiles.empty(), "The LocationPlan canvas emitted "
			+ std::to_string(tiles.size()) + " Sector-family tile(s) ('"
			+ sheetText(tiles) + "') while Layers were bound; the plan is a "
			"World-level tool and keeps the World's sector sheet, which means it "
			"draws no per-Layer Sector artwork at all");
		require(sheets.drawnName() == "SectorAtlas", "The World's own sheet "
			"moved while only Layer bindings were being changed");
	}
}

void render_smoke::registerAtlasInstallPath(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "atlasInstall/everyRegisteredImageSetInstallsFromTheShippedManifest",
		isolated<everyRegisteredImageSetInstallsFromTheShippedManifest> });
	checks.push_back({ "atlasInstall/theInstalledRegionIsTheImageSetsNotTheTilesets",
		isolated<theInstalledRegionIsTheImageSetsNotTheTilesets> });
	checks.push_back({ "atlasInstall/aRegionEscapingTheAtlasImageIsNeverLoadable",
		isolated<aRegionEscapingTheAtlasImageIsNeverLoadable> });
	checks.push_back({ "atlasInstall/aManifestTileTheAuthoredTilesetNeverDefinesIsRefused",
		isolated<aManifestTileTheAuthoredTilesetNeverDefinesIsRefused> });
	checks.push_back({ "atlasInstall/everyViewportAtlasMatchesItsTilesetNameForName",
		isolated<everyViewportAtlasMatchesItsTilesetNameForName> });
	checks.push_back({ "atlasInstall/theFourSlotsInstallThroughTheRenderStacksPass",
		isolated<theFourSlotsInstallThroughTheRenderStacksPass> });
	checks.push_back({ "atlasInstall/aMissingAtlasLeavesItsSlotEmptyAndReported",
		isolated<aMissingAtlasLeavesItsSlotEmptyAndReported> });
	checks.push_back({ "atlasInstall/aBadObjectsAtlasRefusesAMandatoryInstall",
		isolated<aBadObjectsAtlasRefusesAMandatoryInstall> });
	checks.push_back({ "atlasInstall/theParsedRegistryAnswersAndRefuses",
		isolated<theParsedRegistryAnswersAndRefuses> });
	checks.push_back({ "atlasInstall/theAtlasSetRegistryIsParsedOncePerStack",
		isolated<theAtlasSetRegistryIsParsedOncePerStack> });
	checks.push_back({ "atlasInstall/selectingOneAtlasReReadsOnlyItsOwnTileset",
		isolated<selectingOneAtlasReReadsOnlyItsOwnTileset> });
	checks.push_back({ "atlasInstall/everyShippedAgentPoseTileResolvesInTheInstalledAgentAtlas",
		isolated<everyShippedAgentPoseTileResolvesInTheInstalledAgentAtlas> });
	checks.push_back({ "atlasInstall/everyShippedFurnitureTileResolvesInTheInstalledFurnitureAtlas",
		isolated<everyShippedFurnitureTileResolvesInTheInstalledFurnitureAtlas> });
	checks.push_back({ "atlasInstall/installedSectorSheetsAreKeyedByAtlasName",
		isolated<installedSectorSheetsAreKeyedByAtlasName> });
	checks.push_back({ "atlasInstall/aLayerSheetInstalledAlongsideNeverTakesTheDrawnSheet",
		isolated<aLayerSheetInstalledAlongsideNeverTakesTheDrawnSheet> });
	checks.push_back({ "atlasInstall/theDrawPathDrawsTheSheetInstalledLast",
		isolated<theDrawPathDrawsTheSheetInstalledLast> });
	checks.push_back({ "atlasInstall/aDrawHelperDrawsTheSheetItIsHanded",
		isolated<aDrawHelperDrawsTheSheetItIsHanded> });
	checks.push_back({ "atlasInstall/aRefusedSectorSheetInstallReplacesItsEntryAndReports",
		isolated<aRefusedSectorSheetInstallReplacesItsEntryAndReports> });
	checks.push_back({ "atlasInstall/aSectorTriangleCarriesTheSheetItWasDrawnFrom",
		isolated<aSectorTriangleCarriesTheSheetItWasDrawnFrom> });
	checks.push_back({ "atlasInstall/twoSheetsInOneStreamBecomeTwoBatches",
		isolated<twoSheetsInOneStreamBecomeTwoBatches> });
	checks.push_back({ "atlasInstall/eachSheetKeepsItsOwnBatchWhenTheOrderMoves",
		isolated<eachSheetKeepsItsOwnBatchWhenTheOrderMoves> });
	checks.push_back({ "layerImageSet/aBoundLayerAndAnInheritingLayerDrawDifferentSheetsInOneFrame",
		isolated<aBoundLayerAndAnInheritingLayerDrawDifferentSheetsInOneFrame> });
	checks.push_back({ "layerImageSet/aBackSectorThroughAnApertureNeverWearsTheFrontLayersSheet",
		isolated<aBackSectorThroughAnApertureNeverWearsTheFrontLayersSheet> });
	checks.push_back({ "layerImageSet/overrideBeatsTheWorldPickAndInheritFollowsIt",
		isolated<overrideBeatsTheWorldPickAndInheritFollowsIt> });
	checks.push_back({ "layerImageSet/aBindingThatNamesNoInstalledSheetDrawsTheWorldsSheet",
		isolated<aBindingThatNamesNoInstalledSheetDrawsTheWorldsSheet> });
	checks.push_back({ "layerImageSet/theWireframeOverlayDrawsNoTiles",
		isolated<theWireframeOverlayDrawsNoTiles> });
	checks.push_back({ "layerImageSet/theLocationPlanCanvasKeepsTheWorldsSectorSheet",
		isolated<theLocationPlanCanvasKeepsTheWorldsSectorSheet> });
}
