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
#include "ObjectTileset.h"
#include "SectorDrawSheet.h"
#include "SectorTileset.h"
#include "TileArtwork.h"

#include "core/Agent.h"
#include "core/Defines.h"
#include "core/Furniture.h"
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
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
	checks.push_back({ "atlasInstall/theDrawPathDrawsTheSheetInstalledLast",
		isolated<theDrawPathDrawsTheSheetInstalledLast> });
	checks.push_back({ "atlasInstall/aDrawHelperDrawsTheSheetItIsHanded",
		isolated<aDrawHelperDrawsTheSheetItIsHanded> });
	checks.push_back({ "atlasInstall/aRefusedSectorSheetInstallReplacesItsEntryAndReports",
		isolated<aRefusedSectorSheetInstallReplacesItsEntryAndReports> });
}
