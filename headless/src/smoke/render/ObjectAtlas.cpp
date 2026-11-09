// The object atlas loader's contract (#1006): the authored tile grid is the
// artist's layout, not the World cell. A fixture laid out on a grid nothing
// like the current cell must load, and its sprite regions must resolve in that
// grid's own units. What still refuses an atlas is what always refused it: a
// sprite that escapes the image, and a grid that leaves a remainder.
//
// Every number below is artwork - the invented fixture's own pixels - except
// where CORE_CELL_WIDTH_PIXELS / CORE_LEVEL_HEIGHT_PIXELS are named to prove
// the fixture really is unlike the World cell.

#include "Checks.h"

#include "ObjectTileset.h"
#include "WorldDrawList.h"

#include "core/Defines.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// One authored sprite exactly as objects.tileset.yaml spells it: its cell in
	// the authored grid, and its content rectangle inside that cell.
	struct AuthoredSprite
	{
		char const* name;
		int column, row;
		int x, y, width, height;
		bool tintable;
	};

	// The invented atlas: 192 x 120 pixels laid out on a 32 x 40 authored grid,
	// so six cells across and three down. Neither the current World cell
	// (64 x 160) nor a scaled copy of it.
	int const kTileWidth = 32, kTileHeight = 40;
	int const kImageWidth = 192, kImageHeight = 120;

	std::vector<AuthoredSprite> completeSet()
	{
		return {
			{"door", 0, 0, 2, 4, 28, 32, false},
			{"button-enabled", 1, 0, 10, 14, 12, 12, false},
			{"button-disabled", 2, 0, 10, 14, 12, 12, false},
			{"window-clear", 3, 0, 2, 8, 28, 24, false},
			{"window-frosted", 4, 0, 2, 8, 28, 24, false},
			{"window-tinted", 5, 0, 2, 8, 28, 24, false},
			{"booth-window-open", 0, 1, 2, 8, 28, 24, false},
			{"booth-window-closed", 1, 1, 2, 8, 28, 24, false},
			{"agent", 2, 1, 8, 6, 16, 28, true},
			{"marker", 3, 1, 10, 10, 12, 12, true},
			{"platform-lift", 4, 1, 1, 2, 30, 36, false},
			{"shuttle-car-left", 5, 1, 0, 4, 32, 32, false},
			{"shuttle-car-middle", 0, 2, 0, 4, 32, 32, false},
			{"shuttle-car-right", 1, 2, 0, 4, 32, 32, false},
			{"shuttle-car-connector", 2, 2, 4, 12, 24, 16, false},
			{"lift-car", 3, 2, 1, 2, 30, 36, false},
			// Content may start in its own cell and run on into the neighbour,
			// as the bundled atlas does with its wide Lying tile. The loader has
			// never required a content rectangle to stay inside one cell.
			{"human-lying", 4, 2, 0, 14, 40, 26, true},
		};
	}

	ObjectTileset loadFixture(smoke::Context const& context, std::string const& tag,
		int imageWidth, int imageHeight, int tileWidth, int tileHeight,
		std::vector<AuthoredSprite> const& sprites)
	{
		std::ostringstream yaml;
		yaml << "version: 1\n"
			<< "image: invented.png\n"
			<< "size: [" << imageWidth << ", " << imageHeight << "]\n"
			<< "tile_size: [" << tileWidth << ", " << tileHeight << "]\n"
			<< "sprites:\n";
		for (auto const& sprite : sprites)
			yaml << "  " << sprite.name << ":\n"
				<< "    cell: [" << sprite.column << ", " << sprite.row << "]\n"
				<< "    content: [" << sprite.x << ", " << sprite.y << ", "
				<< sprite.width << ", " << sprite.height << "]\n"
				<< "    tintable: " << (sprite.tintable ? "true" : "false") << "\n";
		auto const path = context.temporaryRoot() / (tag + ".tileset.yaml");
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << yaml.str();
		require(bool(out), "Could not write authored-grid fixture: " + path.string());
		out.close();
		return ObjectTileset::load(path);
	}

	// Writes a fixture the loader is expected to refuse and hands back its reason.
	std::string refusal(smoke::Context const& context, std::string const& tag,
		int imageWidth, int imageHeight, int tileWidth, int tileHeight,
		std::vector<AuthoredSprite> const& sprites, char const* expectation)
	{
		try
		{
			loadFixture(context, tag, imageWidth, imageHeight, tileWidth, tileHeight, sprites);
		}
		catch (std::exception const& error)
		{
			return error.what();
		}
		throw std::runtime_error(std::string("Atlas was accepted but should not have been: ") + expectation);
	}

	AuthoredSprite& find(std::vector<AuthoredSprite>& sprites, std::string const& name)
	{
		for (auto& sprite : sprites)
			if (name == sprite.name) return sprite;
		throw std::runtime_error("Fixture sprite missing: " + name);
	}

	void authoredGridNeedNotMatchTheCell(smoke::Context const& context)
	{
		// Were the fixture ever re-cut onto the World cell, this check would stop
		// proving anything without failing, so say so out loud.
		require(kTileWidth != CORE_CELL_WIDTH_PIXELS || kTileHeight != CORE_LEVEL_HEIGHT_PIXELS,
			"The authored-grid fixture must not be laid out on the World cell");

		auto const sprites = completeSet();
		auto const tiles = loadFixture(context, "authored-grid", kImageWidth, kImageHeight,
			kTileWidth, kTileHeight, sprites);
		require(tiles.width == kImageWidth && tiles.height == kImageHeight,
			"Atlas image size must be the authored image size");

		// Cell plus content offset, in the authored grid's own units: never
		// rescaled by anything the World cell happens to be.
		auto const& agent = tiles.sprites.at("agent").region;
		require(agent.x == 2 * kTileWidth + 8 && agent.y == 1 * kTileHeight + 6
			&& agent.width == 16 && agent.height == 28,
			"Sprite region did not resolve in the authored grid's units");
		auto const& lift = tiles.sprites.at("platform-lift").region;
		require(lift.x == 4 * kTileWidth + 1 && lift.y == 1 * kTileHeight + 2
			&& lift.width == 30 && lift.height == 36,
			"A second authored cell did not resolve in the authored grid's units");
		// Content that starts in its cell and runs into the neighbour is kept as
		// authored, not clipped to the cell.
		auto const& lying = tiles.sprites.at("human-lying").region;
		require(lying.x == 4 * kTileWidth && lying.width == 40 && lying.height == 26,
			"Content spanning its authored cell was not preserved");
		for (auto const& [name, sprite] : tiles.sprites)
		{
			auto const& r = sprite.region;
			require(r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0
				&& r.x + r.width <= tiles.width && r.y + r.height <= tiles.height,
				"Resolved region escaped the atlas image: " + name);
		}

		// The resolved region is what reaches the sampler, divided by the image's
		// own size: the authored grid never appears in the recorded command.
		setObjectTileset(tiles, static_cast<TextureId>(1));
		WorldDrawList list(WorldDrawList::ClipRectangle{{-1000, -1000}, {1000, 1000}});
		auto const before = list.commands().size();
		require(drawObjectSprite("agent", &list, {0, 0}, {100, 200}),
			"An authored-grid sprite must draw");
		require(list.commands().size() == before + 2,
			"One sprite image must draw one two-triangle quad");
		auto const& triangle = std::get<WorldDrawList::Triangle>(list.commands()[before]);
		require(triangle.texture == WorldDrawList::Texture::ObjectAtlas,
			"Sprite must draw from the object atlas channel");
		auto near = [](float a, float b) { return std::abs(a - b) < 0.0001f; };
		require(near(triangle.texcoords[0].x, (agent.x + 0.5f) / kImageWidth)
			&& near(triangle.texcoords[0].y, (agent.y + 0.5f) / kImageHeight)
			&& near(triangle.texcoords[2].x, (agent.x + agent.width - 0.5f) / kImageWidth)
			&& near(triangle.texcoords[2].y, (agent.y + agent.height - 0.5f) / kImageHeight),
			"Sprite UVs did not come from the authored region over the authored image");
		// The destination is World geometry and the source is artwork: stretching
		// one into the other is the renderer's job, so a destination of any size
		// is legal against this grid.
		require(drawObjectSprite("platform-lift", &list, {0, 0},
			{3 * CORE_CELL_WIDTH_PIXELS, 2 * CORE_LEVEL_HEIGHT_PIXELS}),
			"An authored-grid sprite must stretch into a World-sized destination");
		clearObjectTileset();
	}

	void escapingContentAndRemaindersAreRefused(smoke::Context const& context)
	{
		// Content starting outside its authored cell: still refused, in either axis.
		auto sideways = completeSet();
		find(sideways, "door").x = kTileWidth;
		auto const escaped = refusal(context, "escaped-x", kImageWidth, kImageHeight,
			kTileWidth, kTileHeight, sideways, "content starting outside its authored cell");
		require(escaped.find("cell") != std::string::npos,
			"Escaped content must be refused by the per-sprite bounds check: " + escaped);

		auto downwards = completeSet();
		find(downwards, "marker").y = kTileHeight;
		require(refusal(context, "escaped-y", kImageWidth, kImageHeight,
			kTileWidth, kTileHeight, downwards, "content starting below its authored cell")
			.find("cell") != std::string::npos,
			"Content starting below its authored cell must be refused");

		// Content that clears its cell but runs off the end of the image.
		auto offImage = completeSet();
		auto& car = find(offImage, "lift-car");
		car.column = 5; car.row = 2; car.x = 10;
		require(refusal(context, "off-image", kImageWidth, kImageHeight,
			kTileWidth, kTileHeight, offImage, "content running past the image edge")
			.find("cell") != std::string::npos,
			"Content running past the image edge must be refused");

		// A grid that does not divide the image leaves a cell that resolves to
		// nothing, so it is refused - and the reason states the real requirement
		// instead of naming a magic pixel size.
		auto const uneven = refusal(context, "uneven-grid", kImageWidth - 2, kImageHeight,
			kTileWidth, kTileHeight, completeSet(), "an authored grid leaving a remainder");
		require(uneven.find("authored") != std::string::npos && uneven.find("tile") != std::string::npos,
			"The rejection must state the authored-grid requirement: " + uneven);
		require(!std::any_of(uneven.begin(), uneven.end(),
			[](char c) { return c >= '0' && c <= '9'; }),
			"The rejection must not name a pixel size: " + uneven);
	}
}

void render_smoke::registerObjectAtlas(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "objectAtlas/authoredGridNeedNotMatchTheCell",
		isolated<authoredGridNeedNotMatchTheCell> });
	checks.push_back({ "objectAtlas/escapingContentAndRemaindersAreRefused",
		isolated<escapingContentAndRemaindersAreRefused> });
}
