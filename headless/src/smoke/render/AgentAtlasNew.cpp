// AgentAtlas carries four Agent appearances, each with all five Poses,
// keyed by appearance rather than by Agent type (#1018).
//
// Human, Android, Cleaning Bot and a generic fallback each own a row of
// five Pose tiles. A Remote operator that looks like a Human names the
// Human tiles and shares that row rather than duplicating five tiles under
// a different name, and a type with no artwork of its own names the
// generic `agent` row - which is a row like any other, so the fallback is
// drawn by the same path as the real appearances.
//
// Agent artwork is drawn as the sheet paints it. Nothing is multiplied
// into an Agent sprite on the way to the screen: an Agent's look is its
// tiles, and a look that has to differ is a different tile.
//
// The artwork is installed the way the render stack installs it - pixel
// regions from the `ImageSet` the application manifest declares, render
// semantics from the authored tileset the atlas-set registry names for it -
// so these checks exercise the same pairing the application does. Every
// assertion is on the recorded WorldDrawList command vector, the stable
// CPU seam ADR 0010 names, or on the refusal a malformed tileset gets at
// the loader seam.

#include "Checks.h"

#include "AgentPoseTestAccess.h"
#include "ObjectTileset.h"
#include "Render.h"
#include "TileArtwork.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "core/Agent.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentType.h"
#include "core/Defines.h"
#include "core/Pose.h"
#include "core/World.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern UISettings gUISettings;
extern core::Agent* gSelectedAgent;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

	std::string const kTileset = "resources/textures/agents-new.tileset.yaml";

	// The Agent atlas the bundle ships. There is one family now: the `New`
	// suffix dropped at the #1013 terminus, so the canonical name is the one
	// every check below installs and every shipped Agent type draws from.
	std::string const kAgentAtlas{ "AgentAtlas" };

	// The four appearances, in sheet order. `agent` is the generic one.
	std::vector<std::string> const kAppearances{ "human", "android",
		"cleaning-bot", "agent" };

	// The five Poses, in the sheet's column order. Lying and Crawling are
	// the two that exceed one authored cell's width.
	std::vector<std::string> const kPoses{ "standing", "sitting", "lying",
		"crawling", "crouching" };

	std::vector<std::string> const kOverWidePoses{ "lying", "crawling" };

	int const kCellWidth = 128;
	int const kOverWideContentWidth = 144;

	std::string tileName(std::string const& appearance, std::string const& pose)
	{
		return appearance + "-" + pose;
	}

	std::string tilePrefix(std::string const& appearance)
	{
		return appearance + "-";
	}

	core::Pose poseFromName(std::string const& name)
	{
		for (auto const pose : { core::Pose::Standing, core::Pose::Sitting,
			core::Pose::Lying, core::Pose::Crouching, core::Pose::Crawling })
			if (name == core::poseName(pose)) return pose;
		throw std::runtime_error("No Pose named " + name);
	}

	// ------------------------------------------------------------------
	// The atlas as the application holds it.
	// ------------------------------------------------------------------

	struct ManifestResource
	{
		bool found{ false };
		std::string type;
		std::string location;
		std::string imageReference;
		std::map<std::string, std::string> options;
		AtlasPixelRegions regions;
	};

	// One entry of the application asset manifest. Each field is guarded on
	// its own because a yaml-cpp node that was never written is a zombie,
	// and subscripting one throws rather than reading as absent.
	ManifestResource manifestResource(YAML::Node const& manifest,
		std::string const& name)
	{
		ManifestResource found;
		for (auto const& resource : manifest["Resources"]["Resource"])
		{
			if (!resource.IsMap()) continue;
			if (resource["name"].as<std::string>() != name) continue;
			found.found = true;
			found.type = resource["type"].as<std::string>();
			if (resource["location"])
				found.location = resource["location"].as<std::string>();
			if (auto const options = resource["Option"])
				for (auto const& option : options)
					found.options[option["name"].as<std::string>()] =
						option["value"].as<std::string>();
			if (auto const dependencies = resource["DependentResources"])
			{
				auto const dependency = dependencies["DependentResource"];
				if (dependency.IsMap() && dependency["ref"])
					found.imageReference = dependency["ref"].as<std::string>();
			}
			if (auto const definitions = resource["Definitions"])
				if (auto const definition = definitions["Definition"])
					if (auto const images = definition["Images"])
						for (auto const& image : images["Image"])
							found.regions.emplace(image["name"].as<std::string>(),
								AtlasPixelRegion{ image["x"].as<int>(),
									image["y"].as<int>(), image["width"].as<int>(),
									image["height"].as<int>() });
		}
		return found;
	}

	YAML::Node bundledManifest(smoke::Context const& context)
	{
		return YAML::LoadFile(context.fixture("resources/Resources.yaml").string());
	}

	// The authored atlas-set registry beside the manifest: which tileset is
	// asked about for which atlas, and in which authored format.
	std::map<std::string, std::string> atlasSetTilesets(
		smoke::Context const& context)
	{
		auto const root = YAML::LoadFile(
			context.fixture("resources/atlas-sets.yaml").string());
		require(root.IsMap() && root["sets"].IsMap(),
			"resources/atlas-sets.yaml has no `sets` map of atlas name to authored "
			"tileset");
		std::map<std::string, std::string> sets;
		for (auto const& entry : root["sets"])
			sets[entry.first.as<std::string>()] =
				entry.second["tileset"].as<std::string>();
		return sets;
	}

	// The PNG's own dimensions, read from its IHDR chunk. The tileset states
	// the sheet size; the committed file has to agree with it, because the
	// pixel regions are cut against that size.
	std::pair<int, int> pngSize(std::filesystem::path const& path)
	{
		std::ifstream file(path, std::ios::binary);
		require(bool(file), "Could not open the atlas image: " + path.string());
		unsigned char header[24]{};
		file.read(reinterpret_cast<char*>(header), sizeof(header));
		require(file.gcount() == static_cast<std::streamsize>(sizeof(header)),
			"The atlas image is too short to carry its header: " + path.string());
		auto const be = [&](int offset) -> unsigned int {
			return (unsigned(header[offset]) << 24) | (unsigned(header[offset + 1]) << 16)
				| (unsigned(header[offset + 2]) << 8) | unsigned(header[offset + 3]);
		};
		require(be(0) == 0x89504e47u && be(12) == 0x49484452u,
			"The atlas image is not a PNG: " + path.string());
		return { int(be(16)), int(be(20)) };
	}

	// Installs one Agent atlas through the one install path the render stack
	// uses: pixel regions from the manifest's `ImageSet`, render semantics
	// from the authored tileset the atlas-set registry names for it. Which
	// sheet is installed is a parameter because the shipped-type pairing is
	// asserted against every `-New` Agent atlas, and the old sheet is
	// installed too to prove its Android art is still there (#1028).
	ObjectTileset installAgentAtlas(smoke::Context const& context,
		std::string const& setName)
	{
		auto const manifest = bundledManifest(context);
		auto const set = manifestResource(manifest, setName);
		require(set.found && set.type == "ImageSet",
			"The application manifest registers no " + setName + " ImageSet");
		auto const tileset = atlasSetTilesets(context)[setName];
		require(!tileset.empty(), "The atlas-set registry names no tileset for "
			+ setName);
		// The registry's paths are relative to the registry file, which sits
		// in `resources/` beside the manifest.
		auto const authored = ObjectTileset::load(
			context.fixture("resources/" + tileset));
		ObjectTileset installed;
		installed.width = authored.width;
		installed.height = authored.height;
		installed.sprites = installAtlasTiles(authored.sprites, set.regions,
			authored.width, authored.height, setName);
		setAgentTileset(installed, static_cast<TextureId>(1));
		return installed;
	}

	ObjectTileset installNewAgentAtlas(smoke::Context const& context)
	{
		return installAgentAtlas(context, kAgentAtlas);
	}

	// ------------------------------------------------------------------
	// Agent types that name the new appearance tiles.
	// ------------------------------------------------------------------

	std::string readFile(std::filesystem::path const& path)
	{
		std::ifstream input(path);
		require(bool(input), "Could not read the Agent type source: " + path.string());
		return { std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>() };
	}

	void replaceAll(std::string& text, std::string const& from,
		std::string const& to)
	{
		size_t at{};
		while ((at = text.find(from, at)) != std::string::npos)
		{
			text.replace(at, from.size(), to);
			at += to.size();
		}
	}

	// A new Agent type wearing one appearance: the bundled Human source with
	// its type identity changed and every Pose tile re-prefixed onto that
	// appearance's row. Nothing else about the type changes, so the only
	// difference between two types made this way is which row they name.
	void attachAppearance(smoke::Context const& context, core::World& world,
		std::string const& typeId, std::string const& appearance)
	{
		auto source = readFile(context.fixture("resources/test-worlds/human.agent.lua"));
		replaceAll(source, "type_id = \"Human\"", "type_id = \"" + typeId + "\"");
		replaceAll(source, tilePrefix("human"), tilePrefix(appearance));
		std::string diagnostic;
		require(world.attachAgentType(typeId + ".agent.lua", source, &diagnostic),
			"Could not attach the " + typeId + " Agent type: " + diagnostic);
	}

	// The bundled Remote operator: a different type, the Human appearance.
	void attachRemoteOperator(smoke::Context const& context, core::World& world)
	{
		auto const source = readFile(
			context.fixture("resources/test-worlds/remote-operator.agent.lua"));
		std::string diagnostic;
		require(world.attachAgentType("remote-operator.agent.lua", source,
			&diagnostic), "Could not attach the Remote operator type: " + diagnostic);
	}

	struct AgentFixture
	{
		std::shared_ptr<core::World> world;
		uint32_t room{};
	};

	// The tag registry is attached before the build is finished, which is
	// when a World resolves it and the tag Colour becomes readable.
	AgentFixture makeFixture(
		std::shared_ptr<core::AgentTagRegistry> registry = nullptr)
	{
		AgentFixture fixture;
		fixture.world = std::make_shared<core::World>("Agent appearances", 8, 2);
		if (registry) fixture.world->attachAgentTagRegistry("appearances.tags.yaml",
			std::move(registry));
		fixture.room = fixture.world->addRoom("Room", 0, 0, 0, 8, 1);
		fixture.world->finishBuild();
		fixture.world->pauseSimulation();
		return fixture;
	}

	struct Spawned
	{
		core::AgentId id{};
		core::Agent* entity{ nullptr };
	};

	Spawned spawn(std::shared_ptr<core::World> const& world, uint32_t room,
		std::string const& typeId, float x)
	{
		Spawned spawned;
		spawned.id = world->createAgent(typeId, typeId, room, 0, x);
		spawned.entity = world->lookupAgent(spawned.id).entity;
		require(spawned.entity != nullptr, "Could not create the " + typeId + " Agent");
		return spawned;
	}

	// ------------------------------------------------------------------
	// Reading the Agent body back out of the command stream.
	// ------------------------------------------------------------------

	struct Body
	{
		std::string tile;
		pf::render::PackedColourValue colour{};
		float width{}, height{};
		bool drawn{ false };
		// The missing-tile fallback: the flat magenta quad `renderAgent`
		// draws when the current Pose tile - or the Standing tile it scales
		// against - does not resolve in the installed atlas. It is not
		// artwork, it is the renderer saying it had nothing to draw, and no
		// shipped Agent type should ever produce it (#1028).
		bool missingTile{ false };
	};

	// The tile one quad started from. The comparison is on the minimum UV,
	// which is the tile's own top-left texel and so names the tile the
	// renderer asked for however much of it reached the screen.
	std::string tileSampledBy(WorldDrawList::Triangle const& triangle,
		ObjectTileset const& atlas)
	{
		float u0 = 1e9f, v0 = 1e9f;
		for (auto const& uv : triangle.texcoords)
		{
			u0 = std::min(u0, uv.x);
			v0 = std::min(v0, uv.y);
		}
		for (auto const& [name, artwork] : atlas.sprites)
		{
			auto const u = (float(artwork.region.x) + 0.5f) / float(atlas.width);
			auto const v = (float(artwork.region.y) + 0.5f) / float(atlas.height);
			if (near(u0, u) && near(v0, v)) return name;
		}
		return {};
	}

	// Draws one Agent and reports the body the renderer recorded.
	Body drawBody(core::Agent const& agent, ObjectTileset const& atlas)
	{
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		renderAgent(&agent, &commands);
		Body body;
		auto const magenta = packColour(255, 0, 255, 255);
		float left = 1e9f, right = -1e9f, bottom = 1e9f, top = -1e9f;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle) continue;
			if (triangle->texture != WorldDrawList::Texture::AgentAtlas)
			{
				if (triangle->colour == magenta) body.missingTile = true;
				continue;
			}
			body.drawn = true;
			body.colour = triangle->colour;
			if (body.tile.empty()) body.tile = tileSampledBy(*triangle, atlas);
			for (auto const& point : triangle->positions)
			{
				left = std::min(left, point.x);
				right = std::max(right, point.x);
				bottom = std::min(bottom, point.y);
				top = std::max(top, point.y);
			}
		}
		body.width = right - left;
		body.height = top - bottom;
		return body;
	}

	// ------------------------------------------------------------------
	// The registration: twenty tiles, the full cross product.
	// ------------------------------------------------------------------
	void twentyTilesAreRegistered(smoke::Context const& context)
	{
		using smoke::require;
		auto const authored = ObjectTileset::load(context.fixture(kTileset));
		require(authored.sprites.size() == kAppearances.size() * kPoses.size(),
			"AgentAtlas authors " + std::to_string(authored.sprites.size())
			+ " tiles; four appearances x five Poses is twenty");
		// The full cross product: no appearance missing a Pose, no Pose
		// missing an appearance.
		for (auto const& appearance : kAppearances)
			for (auto const& pose : kPoses)
				require(authored.sprites.count(tileName(appearance, pose)),
					"AgentAtlas registers no '" + tileName(appearance, pose)
					+ "' tile");

		// The sheet is the 5x4 grid of 128x320 cells the set is sized for.
		require(authored.width == 640 && authored.height == 1280,
			"AgentAtlas is not the 640x1280 sheet its set is sized for");
		auto const sheet = pngSize(context.fixture("resources/textures/agents-new.png"));
		require(sheet.first == authored.width && sheet.second == authored.height,
			"The committed agents-new.png is " + std::to_string(sheet.first) + "x"
			+ std::to_string(sheet.second) + ", the tileset states "
			+ std::to_string(authored.width) + "x"
			+ std::to_string(authored.height));

		// The manifest's generated entries agree with the authored tileset,
		// so the runtime pixel contract cannot drift from what was authored.
		auto const manifest = bundledManifest(context);
		auto const set = manifestResource(manifest, "AgentAtlas");
		require(set.found && set.type == "ImageSet",
			"AgentAtlas is not registered as an ImageSet");
		require(set.imageReference == "AgentAtlasImage",
			"AgentAtlas's paired Image is " + set.imageReference);
		auto const image = manifestResource(manifest, "AgentAtlasImage");
		require(image.found && image.type == "Image"
			&& image.location == "textures/agents-new.png",
			"AgentAtlasImage is not the atlas image the tileset names");
		require(image.options.count("filtering")
				&& image.options.at("filtering") == "none"
				&& image.options.count("uv-style")
				&& image.options.at("uv-style") == "atlas",
			"AgentAtlasImage must be an unfiltered atlas image, as every "
			"viewport atlas is");
		require(set.regions.size() == authored.sprites.size(),
			"The manifest's AgentAtlas entries and the authored tileset "
			"differ in count");
		for (auto const& [name, artwork] : authored.sprites)
		{
			auto const region = set.regions.find(name);
			require(region != set.regions.end(),
				"Authored tile '" + name + "' has no generated ImageSet entry");
			require(region->second.x == artwork.region.x
				&& region->second.y == artwork.region.y
				&& region->second.width == artwork.region.width
				&& region->second.height == artwork.region.height,
				"Generated ImageSet entry for '" + name
				+ "' drifted from the authored tileset");
		}
		// The registry names this tileset for this atlas, as object tiles.
		require(atlasSetTilesets(context)["AgentAtlas"]
			== "textures/agents-new.tileset.yaml",
			"The atlas-set registry names no object tileset for AgentAtlas");
	}

	// ------------------------------------------------------------------
	// Keyed by appearance: two types, one row, no duplication.
	// ------------------------------------------------------------------
	void twoTypesNamingOneAppearanceShareTheirTiles(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewAgentAtlas(context);
		auto fixture = makeFixture();
		attachRemoteOperator(context, *fixture.world);
		auto const human = spawn(fixture.world, fixture.room, "Human", 2.5f);
		auto const operatorAgent = spawn(fixture.world, fixture.room,
			"RemoteOperator", 5.5f);

		// The duplication this keying ends: the atlas has no row of its own
		// for the Remote operator, because it has no appearance of its own.
		for (auto const& pose : kPoses)
			require(!atlas.sprites.count(tileName("remote-operator", pose)),
				"AgentAtlas gained a Remote-operator tile; the Remote operator "
				"wears the Human appearance");

		for (auto const& pose : kPoses)
		{
			core::AgentPoseTestAccess::set(*human.entity, poseFromName(pose));
			core::AgentPoseTestAccess::set(*operatorAgent.entity, poseFromName(pose));
			auto const humanBody = drawBody(*human.entity, atlas);
			auto const operatorBody = drawBody(*operatorAgent.entity, atlas);
			require(humanBody.drawn && operatorBody.drawn,
				"The Human and Remote operator did not both draw for the " + pose
				+ " Pose");
			require(humanBody.tile == tileName("human", pose),
				"The Human drew " + humanBody.tile + " for the " + pose + " Pose");
			require(operatorBody.tile == tileName("human", pose),
				"The Remote operator drew " + operatorBody.tile + " for the " + pose
				+ " Pose instead of sharing the Human row");
			// Same tile, same geometry: sharing is not a re-cut of the art.
			require(near(humanBody.width, operatorBody.width)
					&& near(humanBody.height, operatorBody.height),
				"The two types sharing the " + pose + " tile drew it at different sizes");
		}
	}

	// ------------------------------------------------------------------
	// The generic appearance is the fallback a bare type names.
	// ------------------------------------------------------------------
	void genericAppearanceIsTheFallbackForATypeWithNoArtwork(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewAgentAtlas(context);
		// The generic row exists for every Pose, so a type with no artwork
		// never falls through to the missing-tile magenta.
		for (auto const& pose : kPoses)
			require(atlas.sprites.count(tileName("agent", pose)),
				"The generic appearance has no '" + tileName("agent", pose)
				+ "' tile, so a type with no artwork would fall through");

		auto fixture = makeFixture();
		attachAppearance(context, *fixture.world, "NoArtworkYet", "agent");
		auto const agent = spawn(fixture.world, fixture.room, "NoArtworkYet", 3.5f);
		for (auto const& pose : kPoses)
		{
			core::AgentPoseTestAccess::set(*agent.entity, poseFromName(pose));
			auto const body = drawBody(*agent.entity, atlas);
			require(body.drawn && body.tile == tileName("agent", pose),
				"A type with no artwork drew " + body.tile + " for the " + pose
				+ " Pose instead of the generic appearance");
		}
	}

	// ------------------------------------------------------------------
	// The over-wide Poses resolve across adjacent cells.
	// ------------------------------------------------------------------
	void overWidePoseTilesResolveAcrossAdjacentCells(smoke::Context const& context)
	{
		using smoke::require;
		auto const authored = ObjectTileset::load(context.fixture(kTileset));

		// Each over-wide tile starts inside its own authored cell and ends
		// inside the neighbour's, which is the whole content of "may span
		// adjacent cells" as the loader states it.
		for (auto const& pose : kOverWidePoses)
			for (auto const& appearance : kAppearances)
			{
				auto const& region = authored.sprites.at(tileName(appearance, pose)).region;
				require(region.width == kOverWideContentWidth,
					"'" + tileName(appearance, pose) + "' is "
					+ std::to_string(region.width) + "px wide, not the 144px that "
					"makes it exceed one cell");
				auto const cell = region.x / kCellWidth;
				require(region.x % kCellWidth < kCellWidth,
					"'" + tileName(appearance, pose) + "' does not start inside its "
					"own authored cell");
				require(region.x + region.width > (cell + 1) * kCellWidth,
					"'" + tileName(appearance, pose) + "' stops at its own cell edge "
					"and so never spans the adjacent one");
			}

		// The overhang lands in room, not on top of another tile. Two
		// tiles' art overlapping would read as one corrupted sprite.
		for (auto const& first : authored.sprites)
			for (auto const& second : authored.sprites)
			{
				if (first.first == second.first) continue;
				auto const& a = first.second.region;
				auto const& b = second.second.region;
				auto const overlapX = std::min(a.x + a.width, b.x + b.width)
					- std::max(a.x, b.x);
				auto const overlapY = std::min(a.y + a.height, b.y + b.height)
					- std::max(a.y, b.y);
				require(overlapX <= 0 || overlapY <= 0,
					"'" + first.first + "' and '" + second.first
					+ "' overlap their artwork, so the over-wide Pose has no clear "
					"cell to overhang into");
			}

		// And the renderer draws the whole over-wide region: the body is
		// wider than one World cell, and wider than the Standing body by
		// exactly the ratio of the two tiles' widths.
		auto const atlas = installNewAgentAtlas(context);
		auto fixture = makeFixture();
		auto const agent = spawn(fixture.world, fixture.room, "Human", 3.5f);
		core::AgentPoseTestAccess::set(*agent.entity, core::Pose::Standing);
		auto const standing = drawBody(*agent.entity, atlas);
		require(standing.drawn && standing.tile == "human-standing",
			"The Human Standing body did not draw for the over-wide comparison");
		auto const standingTile =
			authored.sprites.at("human-standing").region.width;

		for (auto const& pose : kOverWidePoses)
		{
			core::AgentPoseTestAccess::set(*agent.entity, poseFromName(pose));
			auto const body = drawBody(*agent.entity, atlas);
			require(body.drawn && body.tile == tileName("human", pose),
				"The " + pose + " Pose did not draw its own tile");
			require(body.width > float(CORE_CELL_WIDTH_PIXELS) * gUISettings.worldZoom,
				"The " + pose + " body is no wider than one World cell, so the "
				"over-wide region was cut back to one cell");
			require(near(body.width, standing.width
					* float(kOverWideContentWidth) / float(standingTile)),
				"The " + pose + " body is not the full over-wide tile scaled with "
				"the Standing tile");
		}
	}

	// ------------------------------------------------------------------
	// The shipped bundle, paired with the atlas.
	// ------------------------------------------------------------------
	//
	// Everything above this line proves the atlas is well-formed and that a
	// type *made* to name a row draws it. None of it could see the bug this
	// ticket is about: #1018 authored the `android-*` row, every check
	// drove a synthetic type re-prefixed onto a row, and the real shipped
	// Android type went on naming `robot-standing` - a tile this sheet has
	// never carried - so every Android drew the missing-tile magenta under
	// the atlas #1023 made the default. The checks below read the shipped
	// Agent types themselves and pair them with the sheet (#1028).

	// The directory the bundled Agent-type resources live in, reached
	// through the fixture API by way of one file that has to be there:
	// `fixture()` resolves regular files only, and the repository root is
	// private to the smoke support module.
	std::filesystem::path bundledAgentTypeDirectory(smoke::Context const& context)
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

	struct ShippedAgentType
	{
		std::string resourceName;
		std::string typeId;
		std::string source;
	};

	// The Agent types the application ships, read out of the bundle rather
	// than listed by hand. A hand-written list goes stale by omission - a
	// type added tomorrow is simply never paired - which is the same class
	// of miss as the one this ticket closes.
	std::vector<ShippedAgentType> shippedAgentTypes(smoke::Context const& context)
	{
		auto const suffix = std::string{ ".agent.lua" };
		std::vector<ShippedAgentType> types;
		for (auto const& entry :
			std::filesystem::directory_iterator(bundledAgentTypeDirectory(context)))
		{
			if (!entry.is_regular_file()) continue;
			auto const name = entry.path().filename().string();
			if (name.size() <= suffix.size()
				|| name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
				continue;
			auto const source = readFile(entry.path());
			types.push_back({ name, declaredTypeId(source, entry.path()), source });
		}
		std::sort(types.begin(), types.end(), [](ShippedAgentType const& a,
			ShippedAgentType const& b) { return a.resourceName < b.resourceName; });
		return types;
	}

	// Attaches one shipped Agent type to a fixture World, or accepts the
	// registration the World already carries. The bundled Human is registered
	// by every World's own constructor from the embed generated from that same
	// resource, so re-attaching it is a duplicate rather than a fresh read.
	// Accepting the existing registration is only honest when it names the
	// bundled resource, which is what the second half of this check insists on.
	//
	// The attach result is taken into a variable before the message is built.
	// Building it inline would let the compiler evaluate the message - and read
	// `diagnostic` - before the call that fills it, which turns every attach
	// failure into a failure that reports nothing.
	void attachShippedType(core::World& world, ShippedAgentType const& type)
	{
		if (world.hasAgentType(type.typeId))
		{
			auto const registered = world.agentTypeResourceName(type.typeId);
			require(registered == type.resourceName, "The World already registers the "
				"shipped " + type.typeId + " Agent type under '" + registered
				+ "' rather than the bundled '" + type.resourceName + "', so the "
				"pairing would be asserted against a different source");
			return;
		}
		std::string diagnostic;
		bool const attached = world.attachAgentType(type.resourceName, type.source,
			&diagnostic);
		require(attached, "Could not attach the shipped " + type.typeId
			+ " Agent type: " + diagnostic);
	}

	// The appearance a tile name claims: everything before its `-<Pose>`
	// suffix. `cleaning-bot-standing` is the `cleaning-bot` row's Standing
	// tile, and the row is the thing a shipped type either wears or leaves
	// orphaned.
	std::string appearanceOf(std::string const& tile)
	{
		for (auto const& pose : kPoses)
		{
			auto const suffix = "-" + pose;
			if (tile.size() > suffix.size()
				&& tile.compare(tile.size() - suffix.size(), suffix.size(), suffix) == 0)
				return tile.substr(0, tile.size() - suffix.size());
		}
		return {};
	}

	// The Agent atlases a shipped Agent type is paired against, read from
	// the atlas-set registry rather than hardcoded so the pairing fails
	// loudly the day the registry stops naming one. Since the #1013 terminus
	// dropped the `New` suffix there is exactly one: `AgentAtlas`. A themed
	// sheet a World points its Agent slot at is that World's own choice and
	// is not what the shipped types are checked against.
	std::vector<std::string> shippedAgentAtlases(smoke::Context const& context)
	{
		std::vector<std::string> names;
		for (auto const& [name, tileset] : atlasSetTilesets(context))
			if (name == "AgentAtlas") names.push_back(name);
		require(!names.empty(), "The atlas-set registry names no AgentAtlas, so "
			"the shipped Agent types have nothing to be paired against");
		return names;
	}

	// Which rows the sheet holds back, and why. A reservation nobody has to
	// justify is where orphaned artwork goes to stay orphaned, so the
	// reason is part of the entry rather than a bare name.
	std::map<std::string, std::string> reservedAppearances(
		smoke::Context const& context)
	{
		auto const root = YAML::LoadFile(context.fixture(kTileset).string());
		std::map<std::string, std::string> reserved;
		auto const node = root["reserved_appearances"];
		if (!node) return reserved;
		require(node.IsMap(), "`reserved_appearances` in " + kTileset
			+ " must be a map of appearance to the reason the row is held back");
		for (auto const& entry : node)
		{
			auto const appearance = entry.first.as<std::string>();
			auto const reason = entry.second.IsScalar()
				? entry.second.as<std::string>() : std::string{};
			require(!reason.empty(), "`reserved_appearances` reserves '" + appearance
				+ "' with no reason; every reserved row has to say why it is not "
				"worn by anything");
			reserved.emplace(appearance, reason);
		}
		return reserved;
	}

	// The rows the shipped Agent types actually name, read off their own
	// declared Pose tiles rather than off any list of who is supposed to
	// wear what. Attaching the real sources is the point: the shipped
	// Android type's tiles are the thing that was wrong, and they are only
	// readable through the type's own frozen baseline.
	std::set<std::string> appearancesWornByShippedTypes(smoke::Context const& context)
	{
		std::set<std::string> worn;
		auto fixture = makeFixture();
		size_t index{};
		for (auto const& type : shippedAgentTypes(context))
		{
			attachShippedType(*fixture.world, type);
			auto const spawned = spawn(fixture.world, fixture.room, type.typeId,
				1.5f + float(index++) * 1.5f);
			for (auto const& [pose, definition] :
				spawned.entity->getPhysicalBaseline().poses)
			{
				auto const appearance = appearanceOf(definition.imageTile);
				require(!appearance.empty(), "The shipped " + type.typeId
					+ " Agent type's " + std::string{ core::poseName(pose) }
					+ " Pose names '" + definition.imageTile + "', a tile named with no "
					+ "appearance row, so the pairing cannot see which row it wears");
				worn.insert(appearance);
			}
		}
		return worn;
	}

	// AC: the Android Agent type names the `android-*` appearance and
	// renders its artwork under AgentAtlas.
	void theShippedAndroidTypeWearsTheAndroidAppearance(smoke::Context const& context)
	{
		auto const atlas = installNewAgentAtlas(context);
		auto fixture = makeFixture();
		// The type identity comes off the bundled source rather than a copy of
		// it kept here, so a rename in the resource moves this check with it
		// instead of leaving it asserting a name nothing uses.
		ShippedAgentType const* androidType{};
		auto const types = shippedAgentTypes(context);
		for (auto const& type : types)
			if (type.resourceName == "android.agent.lua") androidType = &type;
		require(androidType != nullptr,
			"The bundle ships no android.agent.lua Agent type, so there is no "
			"Android appearance to check");
		attachShippedType(*fixture.world, *androidType);
		auto const android = spawn(fixture.world, fixture.room, androidType->typeId,
			2.5f);
		auto const& poses = android.entity->getPhysicalBaseline().poses;
		require(poses.count(core::Pose::Standing),
			"The shipped Android type declares no Standing Pose, which every "
			"renderable Agent type needs to scale its art against");
		for (auto const& [pose, definition] : poses)
		{
			auto const expected = tileName("android", std::string{ core::poseName(pose) });
			require(definition.imageTile == expected, "The shipped Android type names '"
				+ definition.imageTile + "' for the "
				+ std::string{ core::poseName(pose) } + " Pose instead of its own "
				"appearance row's '" + expected + "' tile");
			core::AgentPoseTestAccess::set(*android.entity, pose);
			auto const body = drawBody(*android.entity, atlas);
			require(!body.missingTile && body.drawn && body.tile == expected,
				"The shipped Android Agent drew '" + body.tile + "' under "
				+ kAgentAtlas + " for the " + std::string{ core::poseName(pose) }
				+ " Pose instead of its own android- artwork" + (body.missingTile
					? " - it drew the missing-tile magenta" : ""));
		}
	}

	// AC: every shipped Agent type's declared Pose tiles resolve in every
	// `-New` Agent atlas, and the failure names the type and the tile.
	void everyShippedAgentTypeResolvesInEveryNewAgentAtlas(smoke::Context const& context)
	{
		auto const atlases = shippedAgentAtlases(context);
		auto const types = shippedAgentTypes(context);
		require(types.size() >= 4, "Only " + std::to_string(types.size())
			+ " bundled `.agent.lua` Agent types were found in "
			+ bundledAgentTypeDirectory(context).string() + "; the bundle ships "
			"Human, Android, Cleaning Bot and Remote operator, so the pairing is "
			"being asserted against less than the whole bundle");
		for (auto const& atlasName : atlases)
		{
			auto const atlas = installAgentAtlas(context, atlasName);
			auto fixture = makeFixture();
			size_t index{};
			for (auto const& type : types)
			{
				attachShippedType(*fixture.world, type);
				auto const spawned = spawn(fixture.world, fixture.room, type.typeId,
					1.5f + float(index++) * 1.5f);
				auto const& poses = spawned.entity->getPhysicalBaseline().poses;
				require(!poses.empty(), "The shipped " + type.typeId
					+ " Agent type declares no Poses, so it can never be drawn");
				for (auto const& [pose, definition] : poses)
				{
					auto const who = "The shipped " + type.typeId + " Agent type's "
						+ std::string{ core::poseName(pose) } + " Pose names '";
					require(!definition.imageTile.empty(), who + "' declares no image "
						"tile, which " + atlasName + " cannot resolve");
					core::AgentPoseTestAccess::set(*spawned.entity, pose);
					auto const body = drawBody(*spawned.entity, atlas);
					require(!body.missingTile, who + definition.imageTile + "' which "
						+ atlasName + " does not carry, so that Agent draws the "
						"missing-tile magenta instead of artwork");
					require(body.drawn && body.tile == definition.imageTile, who
						+ definition.imageTile + "' but " + atlasName + " drew '" + body.tile
						+ "' for it instead");
				}
			}
		}
	}

	// AC: every appearance row registered in AgentAtlas is named by at
	// least one shipped Agent type, or is explicitly documented as
	// reserved. An unworn row is artwork someone drew that nothing will
	// ever draw, and nothing notices it missing because nothing asks for
	// it - which is exactly how the `android-*` row stayed empty-handed
	// from #1018 to #1028.
	void everyAppearanceRowIsWornOrReserved(smoke::Context const& context)
	{
		auto const authored = ObjectTileset::load(context.fixture(kTileset));
		auto const worn = appearancesWornByShippedTypes(context);
		auto const reserved = reservedAppearances(context);

		std::set<std::string> rows;
		for (auto const& [name, artwork] : authored.sprites)
		{
			auto const appearance = appearanceOf(name);
			require(!appearance.empty(), "'" + name + "' is authored in " + kTileset
				+ " under a name carrying no `-<Pose>` suffix, so the pairing cannot "
				"read which appearance row it belongs to");
			rows.insert(appearance);
		}

		for (auto const& row : rows)
			require(worn.count(row) || reserved.count(row), "'" + row
				+ "' is an appearance row " + kAgentAtlas + " authors that no "
				"shipped Agent type names, so it is orphaned artwork. Give a shipped "
				"Agent type this appearance, or reserve it under "
				"`reserved_appearances` in " + kTileset + " with the reason it is "
				"held back.");

		// A reservation for a row the sheet no longer authors is a stale
		// claim, and a stale claim is where the next orphan hides.
		for (auto const& [row, reason] : reserved)
			require(rows.count(row), "`reserved_appearances` reserves '" + row
				+ "', which " + kAgentAtlas + " no longer authors. The reason "
				"given - \"" + reason + "\" - no longer describes anything on the "
				"sheet, so the reservation should go with the row.");
	}

}

void render_smoke::registerAgentAtlas(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agentAtlasNew/twentyTilesAreRegistered",
		isolated<twentyTilesAreRegistered> });
	checks.push_back({ "agentAtlasNew/twoTypesNamingOneAppearanceShareTheirTiles",
		isolated<twoTypesNamingOneAppearanceShareTheirTiles> });
	checks.push_back({ "agentAtlasNew/genericAppearanceIsTheFallbackForATypeWithNoArtwork",
		isolated<genericAppearanceIsTheFallbackForATypeWithNoArtwork> });
	checks.push_back({ "agentAtlasNew/overWidePoseTilesResolveAcrossAdjacentCells",
		isolated<overWidePoseTilesResolveAcrossAdjacentCells> });
	checks.push_back({ "agentAtlasNew/theShippedAndroidTypeWearsTheAndroidAppearance",
		isolated<theShippedAndroidTypeWearsTheAndroidAppearance> });
	checks.push_back({ "agentAtlasNew/everyShippedAgentTypeResolvesInEveryNewAgentAtlas",
		isolated<everyShippedAgentTypeResolvesInEveryNewAgentAtlas> });
	checks.push_back({ "agentAtlasNew/everyAppearanceRowIsWornOrReserved",
		isolated<everyAppearanceRowIsWornOrReserved> });
}
