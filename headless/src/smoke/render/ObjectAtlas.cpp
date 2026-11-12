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
	};

	// The invented atlas: 192 x 120 pixels laid out on a 32 x 40 authored grid,
	// so six cells across and three down. Neither the current World cell
	// (64 x 160) nor a scaled copy of it.
	int const kTileWidth = 32, kTileHeight = 40;
	int const kImageWidth = 192, kImageHeight = 120;

	std::vector<AuthoredSprite> completeSet()
	{
		return {
			{"door", 0, 0, 2, 4, 28, 32},
			{"button-enabled", 1, 0, 10, 14, 12, 12},
			{"button-disabled", 2, 0, 10, 14, 12, 12},
			{"window-clear", 3, 0, 2, 8, 28, 24},
			{"window-frosted", 4, 0, 2, 8, 28, 24},
			{"window-tinted", 5, 0, 2, 8, 28, 24},
			{"booth-window-open", 0, 1, 2, 8, 28, 24},
			{"booth-window-closed", 1, 1, 2, 8, 28, 24},
			{"agent", 2, 1, 8, 6, 16, 28},
			{"marker", 3, 1, 10, 10, 12, 12},
			{"platform-lift", 4, 1, 1, 2, 30, 36},
			{"shuttle-car-left", 5, 1, 0, 4, 32, 32},
			{"shuttle-car-middle", 0, 2, 0, 4, 32, 32},
			{"shuttle-car-right", 1, 2, 0, 4, 32, 32},
			{"shuttle-car-connector", 2, 2, 4, 12, 24, 16},
			{"lift-car", 3, 2, 1, 2, 30, 36},
			// Content may start in its own cell and run on into the neighbour,
			// as the bundled atlas does with its wide Lying tile. The loader has
			// never required a content rectangle to stay inside one cell.
			{"human-lying", 4, 2, 0, 14, 40, 26},
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
				<< sprite.width << ", " << sprite.height << "]\n";
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
		bool hasOffset{ true };
		bool hasDrawnSize{ true };
		std::string offsetX{ "0" }, offsetY{ "0" };
		std::string drawnWidth{ "1" }, drawnHeight{ "1" };
		// The variant family the base declares, as text; empty means the key
		// is absent. Text rather than an int so a fixture can write `two`,
		// `1.5` or `-1` and ask the loader which of them it notices (#1053).
		std::string variants{};
	};

	std::string v2Block(V2Sprite const& sprite)
	{
		std::ostringstream out;
		out << "  " << sprite.name << ":\n"
			<< "    cell: [" << sprite.column << ", " << sprite.row << "]\n"
			<< "    content: [" << sprite.x << ", " << sprite.y << ", "
			<< sprite.width << ", " << sprite.height << "]\n";
		if (!sprite.variants.empty())
			out << "    variants: " << sprite.variants << "\n";
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
		sprite.offsetX = "0"; sprite.offsetY = "-0.2";
		sprite.drawnWidth = "1"; sprite.drawnHeight = "0.2";
		return sprite;
	}

	// Writes raw fixture YAML and hands back the loader's refusal of it, or
	// empty when the file was accepted. Used where the shape being asked
	// about - a version 1 sprite carrying a version 2 key - is not one the
	// typed fixture builders can write.
	std::string rawRefusal(smoke::Context const& context, std::string const& tag,
		std::string const& yaml)
	{
		auto const path = context.temporaryRoot() / (tag + ".tileset.yaml");
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << yaml;
		out.close();
		require(bool(out), "Could not write the fixture tileset: " + path.string());
		try
		{
			ObjectTileset::load(path);
		}
		catch (std::exception const& error)
		{
			return error.what();
		}
		return {};
	}

	// The same invented sheet, written as a version 1 tileset: the complete
	// required set, with the key the version 1 format does not take put on
	// the `door` sprite when asked for. Written whole rather than as one
	// bare sprite because the version 1 loader holds its required-tile
	// contract, and the refusal under test has to be the key rather than the
	// shape around it.
	std::string v1SheetWithVariants(bool withVariants)
	{
		std::ostringstream yaml;
		yaml << "version: 1\n"
			<< "image: invented.png\n"
			<< "size: [" << kImageWidth << ", " << kImageHeight << "]\n"
			<< "tile_size: [" << kTileWidth << ", " << kTileHeight << "]\n"
			<< "sprites:\n";
		for (auto const& sprite : completeSet())
		{
			yaml << "  " << sprite.name << ":\n"
				<< "    cell: [" << sprite.column << ", " << sprite.row << "]\n";
			if (withVariants && std::string(sprite.name) == "door")
				yaml << "    variants: 2\n";
			yaml << "    content: [" << sprite.x << ", " << sprite.y << ", "
				<< sprite.width << ", " << sprite.height << "]\n";
		}
		return yaml.str();
	}


	// The same version 1 sheet with one extra sprite wearing a `-Vn` name,
	// in the one cell of the invented grid the complete set leaves empty.
	// Nothing in a version 1 file derives a family - the key is refused -
	// so no name shape is spoken for there, and a legacy custom theme that
	// carries a `door-V1` carries it legitimately (#1069).
	std::string v1SheetWithADerivedShapeName()
	{
		return v1SheetWithVariants(false)
			+ "  door-V1:\n"
			  "    cell: [5, 2]\n"
			  "    content: [0, 0, 32, 40]\n";
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
	// the offset it draws at and the size it draws at, in World units
	// (#1014).
	void versionTwoSemanticsAreReadFromTheFile(smoke::Context const& context)
	{
		auto const tiles = loadV2Fixture(context, "v2-valid", { slab() });
		auto const& sprite = tiles.sprites.at("slab");
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

	// ------------------------------------------------------------------
	// Variant families (#1053): a base sprite states a count and the family
	// descends beneath it in the base's own column.
	// ------------------------------------------------------------------

	// The invented sheet is 192 x 120 on a 32 x 40 grid: six columns, three
	// rows. A family has to live inside that, so the deepest a base can sit
	// and still claim a variant below itself is row 1.
	int const kSheetRows = kImageHeight / kTileHeight;

	// `variants: N` on a base sprite yields `-V1` .. `-VN` directly beneath
	// it, each one the base's own rectangle stepped down the sheet by one
	// authored tile height per step. Everything the variant draws - the
	// content it cuts, the offset it draws at, the size it draws at,
	// whether it takes a tint - is its base's, unchanged, because adding a
	// variant is a picture decision and never a re-declaration of geometry.
	void variantFamiliesAreDerivedBeneathTheirBase(smoke::Context const& context)
	{
		using smoke::require;
		auto base = slab();
		base.column = 1;
		base.variants = "2";
		auto const tiles = loadV2Fixture(context, "v2-variants", { base });
		require(tiles.sprites.size() == 3,
			"A base declaring two variants did not load three sprites: "
				+ std::to_string(tiles.sprites.size()));
		auto const& parent = tiles.sprites.at("slab");
		for (int step = 1; step <= 2; ++step)
		{
			auto const variant = "slab-V" + std::to_string(step);
			require(tiles.sprites.count(variant), variant + " was not derived");
			auto const& derived = tiles.sprites.at(variant);
			require(derived.region.x == parent.region.x,
				variant + " left its base's column");
			require(derived.region.y == parent.region.y + step * kTileHeight,
				variant + " is not one authored tile height per step below its base");
			require(derived.region.width == parent.region.width
					&& derived.region.height == parent.region.height,
				variant + " cuts different content than its base cuts");
			require(derived.offset.x == parent.offset.x
					&& derived.offset.y == parent.offset.y
					&& derived.drawnSize.x == parent.drawnSize.x
					&& derived.drawnSize.y == parent.drawnSize.y,
				variant + " did not inherit its base's render semantics");
		}
		// The count the author stated is kept with the base that stated it,
		// so a caller can size a strip of leaves from the tileset rather
		// than from a constant it would have to keep in step.
		require(tiles.variantCounts.size() == 1
				&& tiles.variantCounts.at("slab") == 2,
			"The declared count was not kept with the base that declared it");
		// A derived tile is not the base of a family, so it holds no count
		// of its own; answering 2 there would invite a family under a family.
		require(!tiles.variantCounts.count("slab-V1"),
			"A derived tile reported a family of its own");
	}

	// A base may declare no family at all, and a family of zero is the same
	// as none: the sheet holds the base and nothing descends from it.
	void aBaseWithNoDeclaredFamilyStandsAlone(smoke::Context const& context)
	{
		using smoke::require;
		auto const plain = loadV2Fixture(context, "v2-no-variants", { slab() });
		require(plain.sprites.size() == 1 && plain.variantCounts.empty(),
			"A base with no `variants` key gained a family");
		auto zero = slab();
		zero.variants = "0";
		auto const none = loadV2Fixture(context, "v2-zero-variants", { zero });
		require(none.sprites.size() == 1,
			"A base declaring zero variants derived a tile anyway");
		require(none.variantCounts.count("slab")
				&& none.variantCounts.at("slab") == 0,
			"A declared count of zero was not recorded as the statement it is");
	}

	// `-Vn` is the derivation's own vocabulary. A sprite authored under one
	// of those names would collide with whatever the rule derives, and which
	// of the two won would come down to file order, so the name is refused
	// before anything is derived - which is what stops a tile silently
	// drawing as something else.
	void aVariantNameAuthoredBesideItsFamilyIsRefused(
		smoke::Context const& context)
	{
		using smoke::require;
		auto base = slab();
		base.column = 0;
		base.variants = "1";
		auto authored = slab();
		authored.name = "slab-V1";
		authored.column = 2;
		auto const reason = v2Refusal(context, "v2-authored-beside-family",
			{ base, authored }, "a `-Vn` name authored beside its own family");
		require(reason.find("slab-V1") != std::string::npos
				&& reason.find("reserves") != std::string::npos,
			"A hand-authored name inside a derived family was accepted: " + reason);
		// The same name authored where no family is declared is refused too:
		// the reservation is the whole `-Vn` shape, not just the cells one
		// family happens to occupy, because a second family could be declared
		// beside it in the next edit.
		auto orphan = slab();
		orphan.name = "slab-V1";
		require(rawRefusal(context, "v2-orphan-variant-name",
				"version: 2\nimage: invented.png\nsize: [192, 120]\n"
				"tile_size: [32, 40]\nsprites:\n" + v2Block(orphan))
				.find("reserves") != std::string::npos,
			"A `-Vn` name authored with no family beside it was accepted");
	}

	// The count is the whole of the author's statement about a family, so
	// it has to say a whole number. A word, a fraction or a boolean is not
	// that, and rounding one into a family - or dropping one - is the loader
	// inventing what the file did not say.
	void aVariantCountThatIsNotAWholeNumberIsRefused(
		smoke::Context const& context)
	{
		using smoke::require;
		for (auto const* value : { "two", "1.5", "true", "[1]" })
		{
			auto base = slab();
			base.variants = value;
			auto const reason = v2Refusal(context,
				std::string("v2-variants-not-whole-") + value,
				{ base }, "a non-integer `variants` count");
			require(reason.find("whole number") != std::string::npos,
				std::string("A non-integer `variants` count (") + value
					+ ") was not refused as one: " + reason);
		}
	}

	// A negative count describes a family climbing above its base, which is
	// not what the column means and not what any cell was laid out for.
	void aNegativeVariantCountIsRefused(smoke::Context const& context)
	{
		using smoke::require;
		auto base = slab();
		base.variants = "-1";
		auto const reason = v2Refusal(context, "v2-variants-negative", { base },
			"a negative `variants` count");
		require(reason.find("negative") != std::string::npos,
			"A negative `variants` count was not refused as one: " + reason);
	}

	// A family that runs past the bottom of the declared sheet describes a
	// tile no cell exists for. The refusal names the base's row and the
	// sheet's row count, because "it does not fit" without both numbers is
	// not something an author can act on.
	void aFamilyThatRunsPastTheSheetIsRefused(smoke::Context const& context)
	{
		using smoke::require;
		auto base = slab();
		base.row = kSheetRows - 1;
		base.variants = "1";
		auto const reason = v2Refusal(context, "v2-family-past-the-sheet",
			{ base }, "a family descending past the last declared row");
		require(reason.find("run past the bottom") != std::string::npos,
			"A family descending past the last declared row was accepted: "
				+ reason);
		require(reason.find("row " + std::to_string(kSheetRows - 1) + " of "
					+ std::to_string(kSheetRows))
				!= std::string::npos,
			"The refusal must name the base's row and the sheet's row count: "
				+ reason);
		// The boundary is inclusive of the last row: a base at row 0 of a
		// three-row sheet may claim rows 1 and 2, and no further.
		auto fills = slab();
		fills.variants = std::to_string(kSheetRows - 1);
		require(loadV2Fixture(context, "v2-family-fills-the-sheet", { fills })
				.sprites.size() == static_cast<std::size_t>(kSheetRows),
			"A family filling the sheet down to its last row was not accepted");
		auto over = slab();
		over.variants = std::to_string(kSheetRows);
		require(v2Refusal(context, "v2-family-one-row-over", { over },
				"a family one row past the last row")
				.find("run past the bottom") != std::string::npos,
			"A family one row past the last row was accepted");
	}

	// The version 1 object format takes no family. It authors no render
	// semantics at all - a v1 tile's drawn size is whatever its content
	// rectangle measures over the grid - so a family stacked beneath such a
	// base is a family nobody sized, and the refusal says so rather than
	// deriving something the format cannot describe (#1053).
	void theVersionOneObjectFormatRefusesVariantFamilies(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const reason = rawRefusal(context, "v1-variants",
			v1SheetWithVariants(true));
		require(!reason.empty(),
			"A version 1 object sprite declaring a variant family was accepted");
		require(reason.find("door") != std::string::npos
				&& reason.find("does not derive") != std::string::npos,
			"The refusal must name the sprite and say the format derives no "
			"families: " + reason);
		// The same version 1 file without the key still loads, so the refusal
		// is the key and not the shape around it.
		auto const without = rawRefusal(context, "v1-no-variants",
			v1SheetWithVariants(false));
		require(without.empty(),
			"The version 1 fixture stopped loading once the `variants` key was "
			"taken out of it: " + without);
	}

	// The version 1 object format has no derivation anywhere, so the `-Vn`
	// shape belongs to nothing in it and is not reserved. The reservation
	// travels with the derivation, not with the shape on its own: hoisting
	// it out unconditionally would have refused a legacy custom theme that
	// was never in the derivation's way (#1069).
	void theVersionOneObjectFormatReservesNoVariantNames(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const reason = rawRefusal(context, "v1-derived-shape-name",
			v1SheetWithADerivedShapeName());
		require(reason.empty(),
			"A version 1 object sprite wearing a `-Vn` name was refused, though "
			"the version 1 format derives no family anywhere and reserves no "
			"name shape: " + reason);
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
	checks.push_back({ "objectAtlas/variantFamiliesAreDerivedBeneathTheirBase",
		isolated<variantFamiliesAreDerivedBeneathTheirBase> });
	checks.push_back({ "objectAtlas/aBaseWithNoDeclaredFamilyStandsAlone",
		isolated<aBaseWithNoDeclaredFamilyStandsAlone> });
	checks.push_back({ "objectAtlas/aVariantNameAuthoredBesideItsFamilyIsRefused",
		isolated<aVariantNameAuthoredBesideItsFamilyIsRefused> });
	checks.push_back({ "objectAtlas/aVariantCountThatIsNotAWholeNumberIsRefused",
		isolated<aVariantCountThatIsNotAWholeNumberIsRefused> });
	checks.push_back({ "objectAtlas/aNegativeVariantCountIsRefused",
		isolated<aNegativeVariantCountIsRefused> });
	checks.push_back({ "objectAtlas/aFamilyThatRunsPastTheSheetIsRefused",
		isolated<aFamilyThatRunsPastTheSheetIsRefused> });
	checks.push_back({ "objectAtlas/theVersionOneObjectFormatRefusesVariantFamilies",
		isolated<theVersionOneObjectFormatRefusesVariantFamilies> });
	checks.push_back({ "objectAtlas/theVersionOneObjectFormatReservesNoVariantNames",
		isolated<theVersionOneObjectFormatReservesNoVariantNames> });
}
