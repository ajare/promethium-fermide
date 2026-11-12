// The single-tile Sector preview draw (#1041).
//
// The World draws a Sector surface by repeating one tile once per World
// cell: it needs a cell size, a zoom, a floor line to anchor on, and it
// crops whatever stands above the ceiling. A palette swatch wants the
// opposite of every one of those things - the whole tile, inside whatever
// box it was handed, with no cell grid in the way - and asking the surface
// path for that would give a swatch the size of one cell, cropped by
// however tall the swatch happened to be.
//
// So this file pins the helper that exists instead:
//
//   * one tile in, one image out. The rect's size and aspect change what
//     the swatch looks like and never how many pieces it comes in;
//   * the whole tile reaches the rect. Nothing is cropped at the top the
//     way a short Sector crops its surface, and nothing is repeated the
//     way a long one repeats it;
//   * a variant tile frames its own row of the sheet and not its base's,
//     which is the one thing a swatch must not get wrong;
//   * with no atlas installed, or no tile under the name asked for, it
//     says so and draws nothing - no magenta, because a swatch is a choice
//     and a choice with no art behind it should not be on screen at all.
//
// Every assertion is on the recorded WorldDrawList command vector, the
// stable CPU seam ADR 0010 names, so nothing here needs a window or a GPU.

#include "Checks.h"

#include "DrawTypes.h"
#include "SectorTileset.h"
#include "TileArtwork.h"
#include "WorldDrawList.h"

#include "core/Defines.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace
{
	using smoke::require;

	bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

	std::string const kTileset = "resources/textures/sectors-new.tileset.yaml";

	// Installs the authored new-family set the way the swatch strip sees
	// it: the loader's own pixel regions and the loader's own variant
	// counts. Real regions rather than invented ones, so a swatch that
	// sampled the wrong row of the sheet is caught against the numbers an
	// artist would see in the file.
	SectorTileset installAuthoredAtlas(smoke::Context const& context)
	{
		auto const atlas = SectorTileset::load(context.fixture(kTileset));
		require(atlas.family == SectorTilesetFamily::New,
			"The authored new set did not install as the New family");
		require(atlas.variantCount("corridor") == 3,
			"The authored set declares no family of three for the Corridor");
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorSurfaceVariantCount("corridor") == 3,
			"The installed atlas does not report the family the file declared");
		return atlas;
	}

	WorldDrawList recording()
	{
		return WorldDrawList{ WorldDrawList::ClipRectangle{ { 0.0f, 0.0f },
			{ 4096.0f, 4096.0f } } };
	}

	// One framed tile as it came out of the command stream: the rectangle it
	// covers and, in sheet pixels rather than fractions, the part of the
	// atlas it samples. Reading the sample back in pixels is what lets a
	// check say "this swatch shows the V2 row" without restating the
	// renderer's half-texel inset as a fraction of a sheet it does not own.
	struct Frame
	{
		float left{}, top{}, right{}, bottom{};
		float sampleLeft{}, sampleTop{}, sampleRight{}, sampleBottom{};
	};

	std::vector<Frame> framesOf(WorldDrawList const& commands,
		SectorTileset const& atlas)
	{
		std::vector<WorldDrawList::Triangle const*> triangles;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (triangle
				&& triangle->texture == WorldDrawList::Texture::SectorAtlas)
				triangles.push_back(triangle);
		}
		require(triangles.size() % 2 == 0,
			"An odd number of SectorAtlas triangles came back, so they are not "
			"the pair one framed tile makes");

		std::vector<Frame> frames;
		for (std::size_t pair = 0; pair < triangles.size(); pair += 2)
		{
			Frame frame;
			frame.left = frame.top = 1e9f;
			frame.right = frame.bottom = -1e9f;
			frame.sampleLeft = frame.sampleTop = 1e9f;
			frame.sampleRight = frame.sampleBottom = -1e9f;
			for (auto const* triangle : { triangles[pair], triangles[pair + 1] })
			{
				for (int corner = 0; corner < 3; ++corner)
				{
					frame.left = std::min(frame.left, triangle->positions[corner].x);
					frame.right = std::max(frame.right, triangle->positions[corner].x);
					frame.top = std::min(frame.top, triangle->positions[corner].y);
					frame.bottom = std::max(frame.bottom, triangle->positions[corner].y);
					frame.sampleLeft = std::min(frame.sampleLeft,
						triangle->texcoords[corner].x * float(atlas.width));
					frame.sampleRight = std::max(frame.sampleRight,
						triangle->texcoords[corner].x * float(atlas.width));
					frame.sampleTop = std::min(frame.sampleTop,
						triangle->texcoords[corner].y * float(atlas.height));
					frame.sampleBottom = std::max(frame.sampleBottom,
						triangle->texcoords[corner].y * float(atlas.height));
				}
			}
			frames.push_back(frame);
		}
		return frames;
	}

	// The framed tile, asserted to be exactly one, covering `rect` and
	// sampling the named tile's whole authored region.
	void requireFramesWholeTile(WorldDrawList const& commands,
		SectorTileset const& atlas, std::string const& tile, float left,
		float top, float right, float bottom)
	{
		auto const frames = framesOf(commands, atlas);
		require(frames.size() == 1,
			tile + " framed into " + std::to_string(frames.size())
				+ " pieces; a swatch is one tile, not a grid of them");
		auto const& frame = frames.front();
		require(near(frame.left, left) && near(frame.top, top)
				&& near(frame.right, right) && near(frame.bottom, bottom),
			tile + " did not land on the rect it was handed");
		// The whole region, to within the half-texel inset the renderer
		// takes at each edge so linear filtering never reads a neighbour's
		// texel.
		auto const& region = atlas.surfaces.at(tile);
		require(near(frame.sampleLeft, float(region.x) + 0.5f)
				&& near(frame.sampleRight, float(region.x + region.width) - 0.5f)
				&& near(frame.sampleTop, float(region.y) + 0.5f)
				&& near(frame.sampleBottom, float(region.y + region.height) - 0.5f),
			tile + " sampled something other than its whole authored region");
	}

	// ------------------------------------------------------------------
	// One tile, whatever the box.
	// ------------------------------------------------------------------

	// An arbitrary rect - no relation to a cell size, no zoom behind it -
	// gets the whole tile inside it.
	void oneTileFramesTheRectItIsGiven(smoke::Context const& context)
	{
		auto const atlas = installAuthoredAtlas(context);
		auto commands = recording();
		require(drawSectorTilePreview("corridor", &commands, { 37.0f, 11.0f },
				{ 165.0f, 73.0f }),
			"The preview refused a tile the installed atlas carries");
		requireFramesWholeTile(commands, atlas, "corridor", 37.0f, 11.0f,
			165.0f, 73.0f);
	}

	// The rect is arbitrary in both axes: a swatch three cells wide and one
	// nine cells tall each come back as one framed tile covering its own
	// box. This is the "arbitrary rect" half of the contract, and the half
	// a repeat could never satisfy.
	void anyRectFramesOneTileAndNotACellGrid(smoke::Context const& context)
	{
		auto const atlas = installAuthoredAtlas(context);
		struct Box { float left, top, right, bottom; };
		std::vector<Box> const boxes{
			{ 0.0f, 0.0f, float(CORE_CELL_WIDTH_PIXELS * 3), 41.0f },
			{ 12.0f, 4.0f, 12.0f + float(CORE_CELL_WIDTH_PIXELS * 8), 9.0f },
			{ 5.0f, 5.0f, 5.0f + 19.0f, 5.0f + float(CORE_LEVEL_HEIGHT_PIXELS * 9) }
		};
		for (auto const& box : boxes)
		{
			auto commands = recording();
			require(drawSectorTilePreview("room", &commands,
					{ box.left, box.top }, { box.right, box.bottom }),
				"The preview refused a tile the installed atlas carries");
			requireFramesWholeTile(commands, atlas, "room", box.left, box.top,
				box.right, box.bottom);
		}
	}

	// The contrast that says the helper is not the World path wearing a
	// different name. Over the same four-cell-wide area the surface repeat
	// emits one quad per cell; the preview emits one quad for the lot, and
	// it covers the whole area rather than one cell of it.
	void itIsOneTileWhereTheWorldRepeatIsFour(smoke::Context const& context)
	{
		auto const atlas = installAuthoredAtlas(context);
		auto const right = float(CORE_CELL_WIDTH_PIXELS * 4);
		auto const floorLine = float(CORE_LEVEL_HEIGHT_PIXELS);

		auto repeated = recording();
		require(drawSectorTileSurface("corridor", &repeated, { 0.0f, 0.0f },
				{ right, floorLine }, 1.0f),
			"The surface repeat refused a tile the installed atlas carries");
		require(framesOf(repeated, atlas).size() == 4,
			"The surface repeat is no longer one tile per World cell, so the "
			"comparison below says nothing");

		auto previewed = recording();
		require(drawSectorTilePreview("corridor", &previewed, { 0.0f, 0.0f },
				{ right, floorLine }),
			"The preview refused a tile the installed atlas carries");
		require(framesOf(previewed, atlas).size() == 1,
			"The preview repeated the tile across its rect instead of framing it");
		requireFramesWholeTile(previewed, atlas, "corridor", 0.0f, 0.0f, right,
			floorLine);
	}

	// A swatch shorter than one cell shows its whole tile where a Sector of
	// the same height shows only the tile's floor end. The crop belongs to
	// the World's floor anchor, and a swatch has no floor.
	void aShortRectIsCroppedByTheWorldAndNotByTheSwatch(
		smoke::Context const& context)
	{
		auto const atlas = installAuthoredAtlas(context);
		auto const shortHeight = float(CORE_LEVEL_HEIGHT_PIXELS) * 0.5f;

		auto cropped = recording();
		require(drawSectorTileSurface("corridor", &cropped,
				{ 0.0f, 0.0f }, { float(CORE_CELL_WIDTH_PIXELS), shortHeight },
				1.0f),
			"The surface repeat refused a tile the installed atlas carries");
		auto const croppedFrames = framesOf(cropped, atlas);
		require(croppedFrames.size() == 1,
			"A half-cell Sector surface should still be one cell wide");
		auto const& region = atlas.surfaces.at("corridor");
		// The repeat lost the top of the tile: its sample starts part-way
		// down the region rather than at the top.
		require(croppedFrames.front().sampleTop
				> float(region.y) + 1.0f,
			"The surface repeat no longer crops a short Sector at its ceiling, "
			"so the contrast below says nothing");

		auto framed = recording();
		require(drawSectorTilePreview("corridor", &framed, { 0.0f, 0.0f },
				{ float(CORE_CELL_WIDTH_PIXELS), shortHeight }),
			"The preview refused a tile the installed atlas carries");
		requireFramesWholeTile(framed, atlas, "corridor", 0.0f, 0.0f,
			float(CORE_CELL_WIDTH_PIXELS), shortHeight);
	}

	// ------------------------------------------------------------------
	// A variant frames its own row.
	// ------------------------------------------------------------------

	// The swatch strip shows a family by name, and every name has to frame
	// the row that family was derived into. A preview that resolved every
	// variant to its base would look right on the base swatch and be wrong
	// on every other one.
	void aVariantFramesItsOwnRowAndNotItsBases(smoke::Context const& context)
	{
		auto const atlas = installAuthoredAtlas(context);
		for (int variant = 1; variant <= 3; ++variant)
		{
			auto const tile = "corridor-V" + std::to_string(variant);
			auto commands = recording();
			require(drawSectorTilePreview(tile, &commands, { 4.0f, 4.0f },
					{ 132.0f, 40.0f }),
				tile + " was refused although the family derived it");
			requireFramesWholeTile(commands, atlas, tile, 4.0f, 4.0f, 132.0f,
				40.0f);
			// And it is not the base's row wearing the variant's name.
			auto const& base = atlas.surfaces.at("corridor");
			auto const& derived = atlas.surfaces.at(tile);
			require(derived.y != base.y,
				tile + " shares its base's row, so this check cannot tell them apart");
		}
	}

	// ------------------------------------------------------------------
	// Nothing to frame.
	// ------------------------------------------------------------------

	// With no atlas installed there is no tile and no sheet to sample. The
	// caller is told, and nothing is drawn - not even the missing-art
	// magenta the World path uses, because a swatch that cannot be drawn
	// should not be on screen rather than on screen in the colour that
	// means broken.
	void noAtlasInstalledFramesNothing(smoke::Context const&)
	{
		clearSectorTileset();
		auto commands = recording();
		require(!drawSectorTilePreview("corridor", &commands, { 0.0f, 0.0f },
				{ 128.0f, 36.0f }),
			"The preview claimed to frame a tile with no atlas installed");
		require(commands.commands().empty(),
			"The preview drew something with no atlas installed");
	}

	// A name the installed atlas does not carry is refused the same way,
	// and draws nothing at all: no image, and no magenta rectangle standing
	// in for the artwork that is not there.
	void aTileTheAtlasDoesNotCarryIsRefusedWithoutMagenta(
		smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		auto commands = recording();
		require(!drawSectorTilePreview("corridor-V9", &commands,
				{ 0.0f, 0.0f }, { 128.0f, 36.0f }),
			"The preview framed a variant the family never derived");
		require(commands.commands().empty(),
			"The preview drew a placeholder for a tile that does not exist; a "
			"swatch with no art behind it must not be drawn at all");
	}

	// A rect with no area has nothing to frame, but the tile was found. The
	// caller gets a yes with no commands rather than a refusal that would
	// read as a missing tile.
	void aRectWithNoAreaIsFramedWithNothingInIt(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		auto commands = recording();
		require(drawSectorTilePreview("corridor", &commands, { 20.0f, 30.0f },
				{ 20.0f, 90.0f }),
			"A zero-width rect over a tile that exists was refused outright");
		require(commands.commands().empty(),
			"A zero-width rect emitted geometry it has no area to fill");
	}

	// The corners may arrive either way round. A swatch rect handed over with
	// its corners swapped still frames the tile inside the box they span
	// rather than drawing an inverted nothing.
	void cornersTheOtherWayRoundStillFrameTheSameBox(
		smoke::Context const& context)
	{
		auto const atlas = installAuthoredAtlas(context);
		auto commands = recording();
		require(drawSectorTilePreview("lift", &commands, { 200.0f, 80.0f },
				{ 60.0f, 20.0f }),
			"The preview refused a rect whose corners were handed over swapped");
		requireFramesWholeTile(commands, atlas, "lift", 60.0f, 20.0f, 200.0f,
			80.0f);
	}
}

namespace render_smoke
{
	void registerSectorTilePreview(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "sectorTilePreview/oneTileFramesTheRectItIsGiven",
			isolated<oneTileFramesTheRectItIsGiven> });
		checks.push_back({ "sectorTilePreview/anyRectFramesOneTileAndNotACellGrid",
			isolated<anyRectFramesOneTileAndNotACellGrid> });
		checks.push_back({ "sectorTilePreview/itIsOneTileWhereTheWorldRepeatIsFour",
			isolated<itIsOneTileWhereTheWorldRepeatIsFour> });
		checks.push_back({ "sectorTilePreview/aShortRectIsCroppedByTheWorldAndNotByTheSwatch",
			isolated<aShortRectIsCroppedByTheWorldAndNotByTheSwatch> });
		checks.push_back({ "sectorTilePreview/aVariantFramesItsOwnRowAndNotItsBases",
			isolated<aVariantFramesItsOwnRowAndNotItsBases> });
		checks.push_back({ "sectorTilePreview/noAtlasInstalledFramesNothing",
			isolated<noAtlasInstalledFramesNothing> });
		checks.push_back({ "sectorTilePreview/aTileTheAtlasDoesNotCarryIsRefusedWithoutMagenta",
			isolated<aTileTheAtlasDoesNotCarryIsRefusedWithoutMagenta> });
		checks.push_back({ "sectorTilePreview/aRectWithNoAreaIsFramedWithNothingInIt",
			isolated<aRectWithNoAreaIsFramedWithNothingInIt> });
		checks.push_back({ "sectorTilePreview/cornersTheOtherWayRoundStillFrameTheSameBox",
			isolated<cornersTheOtherWayRoundStillFrameTheSameBox> });
	}
}
