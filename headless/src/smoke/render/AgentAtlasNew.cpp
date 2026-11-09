// AgentAtlasNew carries four Agent appearances, each with all five Poses,
// keyed by appearance rather than by Agent type (#1018).
//
// Human, Android, Cleaning Bot and a generic fallback each own a row of
// five Pose tiles. A Remote operator that looks like a Human names the
// Human tiles and shares that row rather than duplicating five tiles under
// a different name, and a type with no artwork of its own names the
// generic `agent` row - which is a row like any other, so the fallback is
// drawn by the same path as the real appearances.
//
// Tinting is an authored fact. The rule that used to decide whether an
// Agent tile took the Agent colour by matching its name against `agent`,
// `human-*`, `robot-standing` and `cleaning-bot-standing` is deleted in
// this slice, so `tintable` in the tileset is the only thing that says it.
// The regression check here installs a tile named like the Human appearance
// but authored untintable, and a tile named like no appearance at all but
// authored tintable, and shows the flag wins in both directions.
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
#include <fstream>
#include <iterator>
#include <map>
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

	// Installs the new Agent atlas through the one install path the render
	// stack uses: pixel regions from the `ImageSet`, render semantics from
	// the authored tileset.
	ObjectTileset installNewAgentAtlas(smoke::Context const& context)
	{
		auto const manifest = bundledManifest(context);
		auto const set = manifestResource(manifest, "AgentAtlasNew");
		require(set.found && set.type == "ImageSet",
			"The application manifest registers no AgentAtlasNew ImageSet");
		require(atlasSetTilesets(context)["AgentAtlasNew"]
			== "textures/agents-new.tileset.yaml",
			"The atlas-set registry does not name agents-new.tileset.yaml for "
			"AgentAtlasNew");
		auto const authored = ObjectTileset::load(context.fixture(kTileset));
		ObjectTileset installed;
		installed.width = authored.width;
		installed.height = authored.height;
		installed.sprites = installAtlasTiles(authored.sprites, set.regions,
			authored.width, authored.height, "AgentAtlasNew");
		setAgentTileset(installed, static_cast<TextureId>(1));
		return installed;
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
		float left = 1e9f, right = -1e9f, bottom = 1e9f, top = -1e9f;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::AgentAtlas)
				continue;
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
			"AgentAtlasNew authors " + std::to_string(authored.sprites.size())
			+ " tiles; four appearances x five Poses is twenty");
		// The full cross product: no appearance missing a Pose, no Pose
		// missing an appearance.
		for (auto const& appearance : kAppearances)
			for (auto const& pose : kPoses)
				require(authored.sprites.count(tileName(appearance, pose)),
					"AgentAtlasNew registers no '" + tileName(appearance, pose)
					+ "' tile");

		// The sheet is the 5x4 grid of 128x320 cells the set is sized for.
		require(authored.width == 640 && authored.height == 1280,
			"AgentAtlasNew is not the 640x1280 sheet its set is sized for");
		auto const sheet = pngSize(context.fixture("resources/textures/agents-new.png"));
		require(sheet.first == authored.width && sheet.second == authored.height,
			"The committed agents-new.png is " + std::to_string(sheet.first) + "x"
			+ std::to_string(sheet.second) + ", the tileset states "
			+ std::to_string(authored.width) + "x"
			+ std::to_string(authored.height));

		// Every tile is grayscale art that takes the Agent colour.
		for (auto const& [name, artwork] : authored.sprites)
			require(artwork.tintable,
				"'" + name + "' must be authored tintable; Agent art is grayscale");

		// The manifest's generated entries agree with the authored tileset,
		// so the runtime pixel contract cannot drift from what was authored.
		auto const manifest = bundledManifest(context);
		auto const set = manifestResource(manifest, "AgentAtlasNew");
		require(set.found && set.type == "ImageSet",
			"AgentAtlasNew is not registered as an ImageSet");
		require(set.imageReference == "AgentAtlasNewImage",
			"AgentAtlasNew's paired Image is " + set.imageReference);
		auto const image = manifestResource(manifest, "AgentAtlasNewImage");
		require(image.found && image.type == "Image"
			&& image.location == "textures/agents-new.png",
			"AgentAtlasNewImage is not the atlas image the tileset names");
		require(image.options.count("filtering")
				&& image.options.at("filtering") == "none"
				&& image.options.count("uv-style")
				&& image.options.at("uv-style") == "atlas",
			"AgentAtlasNewImage must be an unfiltered atlas image, as every "
			"viewport atlas is");
		require(set.regions.size() == authored.sprites.size(),
			"The manifest's AgentAtlasNew entries and the authored tileset "
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
		require(atlasSetTilesets(context)["AgentAtlasNew"]
			== "textures/agents-new.tileset.yaml",
			"The atlas-set registry names no object tileset for AgentAtlasNew");
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
				"AgentAtlasNew gained a Remote-operator tile; the Remote operator "
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
	// The tint is authored. The name says nothing.
	// ------------------------------------------------------------------

	// The fixture the regression needs: one tile named like the Human
	// appearance but authored untintable, and one named like no appearance
	// at all but authored tintable. Under the name rule this slice deletes
	// the first would tint and the second would not; the authored flags say
	// the opposite of both, and the authored flags are what the renderer
	// now reads.
	std::string tintFixtureYaml()
	{
		return "version: 2\n"
			"image: invented.png\n"
			"size: [256, 320]\n"
			"tile_size: [128, 320]\n"
			"sprites:\n"
			"  human-standing:\n"
			"    cell: [0, 0]\n"
			"    content: [0, 176, 52, 144]\n"
			"    offset: [0.0, 0.0]\n"
			"    drawn_size: [0.40625, 0.45]\n"
			"    tintable: false\n"
			"  security-bot-standing:\n"
			"    cell: [1, 0]\n"
			"    content: [0, 176, 52, 144]\n"
			"    offset: [0.0, 0.0]\n"
			"    drawn_size: [0.40625, 0.45]\n"
			"    tintable: true\n";
	}

	void tintFollowsTheAuthoredFlagNotTheName(smoke::Context const& context)
	{
		using smoke::require;
		auto const path = context.temporaryRoot() / "agent-tint.tileset.yaml";
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << tintFixtureYaml();
		out.close();
		require(bool(out), "Could not write the fixture tileset: " + path.string());

		// Installed through the same seam the application uses, so the flag
		// travels the same road from file to command that the real atlas's does.
		auto const authored = ObjectTileset::load(path);
		AtlasPixelRegions regions;
		for (auto const& [name, artwork] : authored.sprites)
			regions.emplace(name, AtlasPixelRegion{ artwork.region.x, artwork.region.y,
				artwork.region.width, artwork.region.height });
		ObjectTileset installed;
		installed.width = authored.width;
		installed.height = authored.height;
		installed.sprites = installAtlasTiles(authored.sprites, regions,
			authored.width, authored.height, "AgentTintFixture");
		require(!installed.sprites.at("human-standing").tintable
				&& installed.sprites.at("security-bot-standing").tintable,
			"The install lost an authored tintable flag");
		setAgentTileset(installed, static_cast<TextureId>(1));

		auto fixture = makeFixture();
		attachAppearance(context, *fixture.world, "UntintedHumanLookalike", "human");
		attachAppearance(context, *fixture.world, "TintedNewLook", "security-bot");
		auto const untinted = spawn(fixture.world, fixture.room,
			"UntintedHumanLookalike", 2.5f);
		auto const tinted = spawn(fixture.world, fixture.room, "TintedNewLook", 5.5f);
		std::string diagnostic;
		require(fixture.world->setAgentIndividualColour(tinted.id,
			core::AgentColour{ 200, 120, 20 }, &diagnostic),
			"Could not colour the tinted fixture Agent: " + diagnostic);

		// Named like a tile the deleted rule tinted, authored untintable:
		// it draws in the sheet's own colours and takes no tint.
		auto const untintedBody = drawBody(*untinted.entity, installed);
		require(untintedBody.drawn, "The untinted fixture Agent drew no body at all");
		require(untintedBody.colour == White,
			"A tile named 'human-standing' tinted against its authored flag");

		// Named like nothing the deleted rule knew, authored tintable: it
		// takes the resolved Agent colour just like any other Agent tile.
		auto const tintedBody = drawBody(*tinted.entity, installed);
		require(tintedBody.drawn, "The tinted fixture Agent drew no body at all");
		require(tintedBody.colour == agentRenderColour(*tinted.entity, false)
				&& tintedBody.colour == packColour(200, 120, 20),
			"A tile named 'security-bot-standing' failed to tint to its authored flag");
	}

	// ------------------------------------------------------------------
	// Agent colour, tag overrides and selection still drive the tint.
	// ------------------------------------------------------------------
	void agentColourTagOverrideAndSelectionStillDriveTheTint(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewAgentAtlas(context);

		auto registry = core::AgentTagRegistry::create();
		auto const amber = registry->addAgentTag("amber");
		std::string diagnostic;
		require(registry->addAgentTagColour(amber, &diagnostic), diagnostic);
		require(registry->setAgentTagColour(amber, { 200, 120, 20 }, &diagnostic),
			diagnostic);
		auto fixture = makeFixture(registry);

		auto const plain = spawn(fixture.world, fixture.room, "Human", 1.5f);
		auto const tagged = spawn(fixture.world, fixture.room, "Human", 3.5f);
		auto const individual = spawn(fixture.world, fixture.room, "Human", 5.5f);
		require(fixture.world->assignAgentTag(tagged.id, amber, &diagnostic),
			"Could not tag the fixture Agent: " + diagnostic);
		require(fixture.world->setAgentIndividualColour(individual.id,
			core::AgentColour{ 12, 34, 56 }, &diagnostic),
			"Could not colour the fixture Agent: " + diagnostic);

		// The Agent's own resolved colour reaches the sheet.
		auto const plainBody = drawBody(*plain.entity, atlas);
		require(plainBody.drawn && plainBody.tile == "human-standing",
			"The plain Agent did not draw its Human tile");
		require(plainBody.colour == agentRenderColour(*plain.entity, false),
			"The Agent's resolved colour no longer drives the tint");

		// A Colour tag overrides it, and the override reaches the sheet.
		auto const taggedBody = drawBody(*tagged.entity, atlas);
		require(taggedBody.colour == packColour(200, 120, 20)
				&& taggedBody.colour == agentRenderColour(*tagged.entity, false),
			"A Colour tag override no longer drives the Agent tint");

		// An individual colour wins over the tag, as it always did.
		auto const individualBody = drawBody(*individual.entity, atlas);
		require(individualBody.colour == packColour(12, 34, 56)
				&& individualBody.colour == agentRenderColour(*individual.entity, false),
			"An individual Agent colour no longer drives the Agent tint");

		// Selection gold overrides all of them.
		struct SelectionScope
		{
			core::Agent* previous{ gSelectedAgent };
			~SelectionScope() { gSelectedAgent = previous; }
		} selection;
		gSelectedAgent = tagged.entity;
		auto const selectedBody = drawBody(*tagged.entity, atlas);
		require(selectedBody.colour
				== packColour(core::SelectedAgentColour.r, core::SelectedAgentColour.g,
					core::SelectedAgentColour.b)
				&& selectedBody.colour == agentRenderColour(*tagged.entity, true),
			"The selection highlight no longer drives the Agent tint");
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
}

void render_smoke::registerAgentAtlasNew(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agentAtlasNew/twentyTilesAreRegistered",
		isolated<twentyTilesAreRegistered> });
	checks.push_back({ "agentAtlasNew/twoTypesNamingOneAppearanceShareTheirTiles",
		isolated<twoTypesNamingOneAppearanceShareTheirTiles> });
	checks.push_back({ "agentAtlasNew/genericAppearanceIsTheFallbackForATypeWithNoArtwork",
		isolated<genericAppearanceIsTheFallbackForATypeWithNoArtwork> });
	checks.push_back({ "agentAtlasNew/tintFollowsTheAuthoredFlagNotTheName",
		isolated<tintFollowsTheAuthoredFlagNotTheName> });
	checks.push_back({ "agentAtlasNew/agentColourTagOverrideAndSelectionStillDriveTheTint",
		isolated<agentColourTagOverrideAndSelectionStillDriveTheTint> });
	checks.push_back({ "agentAtlasNew/overWidePoseTilesResolveAcrossAdjacentCells",
		isolated<overWidePoseTilesResolveAcrossAdjacentCells> });
}
