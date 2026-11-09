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

	// A version 2 authored sprite: the version 1 fields plus the render
	// offset and the drawn size the format gained (#1014). Every field is
	// spelled as text so a fixture can write `.nan` or leave a field out
	// entirely, which is what the loader is being asked to notice.
	struct V2Sprite
	{
		std::string name;
		int column{}, row{};
		int x{}, y{}, width{}, height{};
		bool tintable{};
		bool hasOffset{ true };
		bool hasDrawnSize{ true };
		std::string offsetX{ "0" }, offsetY{ "0" };
		std::string drawnWidth{ "1" }, drawnHeight{ "1" };
	};

	std::string v2Block(V2Sprite const& sprite)
	{
		std::ostringstream out;
		out << "  " << sprite.name << ":\n"
			<< "    cell: [" << sprite.column << ", " << sprite.row << "]\n"
			<< "    content: [" << sprite.x << ", " << sprite.y << ", "
			<< sprite.width << ", " << sprite.height << "]\n"
			<< "    tintable: " << (sprite.tintable ? "true" : "false") << "\n";
		if (sprite.hasOffset)
			out << "    offset: [" << sprite.offsetX << ", " << sprite.offsetY << "]\n";
		if (sprite.hasDrawnSize)
			out << "    drawn_size: [" << sprite.drawnWidth << ", " << sprite.drawnHeight
				<< "]\n";
		return out.str();
	}

	// The same invented 192 x 120 image on the same 32 x 40 authored grid,
	// written as a version 2 tileset.
	ObjectTileset loadV2Fixture(smoke::Context const& context, std::string const& tag,
		std::vector<V2Sprite> const& sprites, int imageWidth = kImageWidth,
		int imageHeight = kImageHeight, int tileWidth = kTileWidth,
		int tileHeight = kTileHeight)
	{
		std::ostringstream yaml;
		yaml << "version: 2\n"
			<< "image: invented.png\n"
			<< "size: [" << imageWidth << ", " << imageHeight << "]\n"
			<< "tile_size: [" << tileWidth << ", " << tileHeight << "]\n"
			<< "sprites:\n";
		for (auto const& sprite : sprites) yaml << v2Block(sprite);
		auto const path = context.temporaryRoot() / (tag + ".v2.tileset.yaml");
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << yaml.str();
		require(bool(out), "Could not write version 2 fixture: " + path.string());
		out.close();
		return ObjectTileset::load(path);
	}

	std::string v2Refusal(smoke::Context const& context, std::string const& tag,
		std::vector<V2Sprite> const& sprites, char const* expectation)
	{
		try
		{
			loadV2Fixture(context, tag, sprites);
		}
		catch (std::exception const& error)
		{
			return error.what();
		}
		throw std::runtime_error(std::string("Version 2 atlas was accepted but should not "
			"have been: ") + expectation);
	}

	V2Sprite slab()
	{
		V2Sprite sprite;
		sprite.name = "slab";
		sprite.column = 0; sprite.row = 0;
		sprite.x = 0; sprite.y = 0; sprite.width = 32; sprite.height = 8;
		sprite.tintable = true;
		sprite.offsetX = "0"; sprite.offsetY = "-0.2";
		sprite.drawnWidth = "1"; sprite.drawnHeight = "0.2";
		return sprite;
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

	// A version 2 tileset authors the render semantics the renderer reads:
	// the offset it draws at and the size it draws at, in World units, plus
	// the tintable flag that was always meant to be the only source of
	// tintability (#1014).
	void versionTwoSemanticsAreReadFromTheFile(smoke::Context const& context)
	{
		auto const tiles = loadV2Fixture(context, "v2-valid", { slab() });
		auto const& sprite = tiles.sprites.at("slab");
		require(sprite.tintable, "The tintable flag was not read from the file");
		auto near = [](float a, float b) { return std::abs(a - b) < 0.0001f; };
		require(near(sprite.offset.x, 0.0f) && near(sprite.offset.y, -0.2f),
			"The render offset was not read from the file");
		require(near(sprite.drawnSize.x, 1.0f) && near(sprite.drawnSize.y, 0.2f),
			"The drawn size was not read from the file");
		// The pixel region still resolves in the authored grid's own units.
		require(sprite.region.x == 0 && sprite.region.y == 0
			&& sprite.region.width == 32 && sprite.region.height == 8,
			"The content rectangle stopped resolving in the authored grid");
	}

	// A tile with no render offset is refused. A default of zero would be a
	// claim no author made, and it is exactly the claim that would put a
	// Walkway back on the floor line.
	void missingRenderOffsetIsRefused(smoke::Context const& context)
	{
		auto missing = slab();
		missing.hasOffset = false;
		auto const reason = v2Refusal(context, "v2-no-offset", { missing },
			"a tile with no render offset");
		require(reason.find("offset") != std::string::npos,
			"The refusal must name the missing render offset: " + reason);
	}

	// A non-finite offset is refused rather than carried into a destination
	// rectangle, where it would silently become nothing or everything.
	void nonFiniteRenderOffsetIsRefused(smoke::Context const& context)
	{
		for (auto const* value : { ".nan", ".inf", "-.inf" })
		{
			auto nonFinite = slab();
			nonFinite.offsetY = value;
			auto const reason = v2Refusal(context, std::string("v2-offset-") + value,
				{ nonFinite }, "a non-finite render offset");
			require(reason.find("finite") != std::string::npos,
				std::string("A non-finite render offset must be refused as non-finite (")
				+ value + "): " + reason);
		}
		auto sideways = slab();
		sideways.offsetX = ".nan";
		require(v2Refusal(context, "v2-offset-x-nan", { sideways },
			"a non-finite x render offset").find("finite") != std::string::npos,
			"A non-finite x render offset must be refused too");
	}

	// A drawn size that cannot be expressed inside the atlas image is
	// refused: one World unit of authored art is one authored tile, so a
	// drawn size wider or taller than the whole image could never be drawn
	// from it.
	void drawnSizeThatDoesNotFitTheImageIsRefused(smoke::Context const& context)
	{
		// 8 World units across a 32px tile is 256px in a 192px image.
		auto tooWide = slab();
		tooWide.drawnWidth = "8";
		auto const wide = v2Refusal(context, "v2-drawn-too-wide", { tooWide },
			"a drawn size wider than the atlas image");
		require(wide.find("drawn size") != std::string::npos
			&& wide.find("fit") != std::string::npos,
			"The refusal must state that the drawn size does not fit: " + wide);

		// 4 World units down a 40px tile is 160px in a 120px image.
		auto tooTall = slab();
		tooTall.drawnHeight = "4";
		require(v2Refusal(context, "v2-drawn-too-tall", { tooTall },
			"a drawn size taller than the atlas image")
			.find("drawn size") != std::string::npos,
			"A drawn size taller than the image must be refused");

		// A drawn size of zero or less is refused as not positive.
		auto zero = slab();
		zero.drawnWidth = "0";
		require(v2Refusal(context, "v2-drawn-zero", { zero },
			"a zero drawn size").find("positive") != std::string::npos,
			"A zero drawn size must be refused as not positive");

		auto negative = slab();
		negative.drawnHeight = "-1";
		require(v2Refusal(context, "v2-drawn-negative", { negative },
			"a negative drawn size").find("positive") != std::string::npos,
			"A negative drawn size must be refused as not positive");

		auto nonFinite = slab();
		nonFinite.drawnWidth = ".nan";
		require(v2Refusal(context, "v2-drawn-nan", { nonFinite },
			"a non-finite drawn size").find("finite") != std::string::npos,
			"A non-finite drawn size must be refused as non-finite");

		// A missing drawn size is refused as loudly as a missing offset.
		auto absent = slab();
		absent.hasDrawnSize = false;
		require(v2Refusal(context, "v2-no-drawn-size", { absent },
			"a tile with no drawn size").find("drawn_size") != std::string::npos,
			"The refusal must name the missing drawn size");
	}
}

void render_smoke::registerObjectAtlas(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "objectAtlas/authoredGridNeedNotMatchTheCell",
		isolated<authoredGridNeedNotMatchTheCell> });
	checks.push_back({ "objectAtlas/escapingContentAndRemaindersAreRefused",
		isolated<escapingContentAndRemaindersAreRefused> });
	checks.push_back({ "objectAtlas/versionTwoSemanticsAreReadFromTheFile",
		isolated<versionTwoSemanticsAreReadFromTheFile> });
	checks.push_back({ "objectAtlas/missingRenderOffsetIsRefused",
		isolated<missingRenderOffsetIsRefused> });
	checks.push_back({ "objectAtlas/nonFiniteRenderOffsetIsRefused",
		isolated<nonFiniteRenderOffsetIsRefused> });
	checks.push_back({ "objectAtlas/drawnSizeThatDoesNotFitTheImageIsRefused",
		isolated<drawnSizeThatDoesNotFitTheImageIsRefused> });
}
