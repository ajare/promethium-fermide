// A Sector wears a variant where one was recorded, and its base everywhere
// else (#1040).
//
// #1037 pinned the geometry of a Sector surface: one tile per World cell,
// anchored on the floor line, a partial top cell cropped at its top. #1039
// pinned the seam that asks which tile each of those cells wears. This file
// pins what happens when the answer stops being the same for every cell -
// which is the whole of what a variant is, and the whole of what it is not:
//
//   * a cell recorded as wearing variant N draws the type's `-VN` tile,
//     on the same rectangle it drew its base tile on, at the same crop and
//     the same tint;
//   * every cell with no entry draws the base, exactly as it did before any
//     cell could be recorded at all;
//   * a recorded variant the installed atlas has no tile for draws the base
//     and says nothing. The author picked a number from a family the sheet
//     they are looking at has no row for; the honest picture of that is
//     the Sector's own tile, not a hole;
//   * a base tile that is itself missing still draws the missing-art
//     magenta. Falling back from a variant to a base that is not there is
//     not falling back to something;
//   * a World repointed at the version 1 family - which declares no
//     families, and so has no `-Vn` tile under any name - draws base on
//     every variant cell while the Sector keeps its entries, so repointing
//     it back restores the picture with nothing rewritten.
//
// The World in every case here is one that was authored rather than
// assembled: a plain World is serialized, a `surfaceVariants` block is
// written into its Room record the way a hand-authored file carries one,
// and the document is loaded back. That is the path the ticket names - a
// World file saying "this cell wears this variant" - and it is the path
// that has to work before any editor UI exists to write it.
//
// The atlas is a synthetic one, so the check knows exactly which variant
// names exist and which do not, and can tell a base tile from a variant
// tile by the region its quad samples rather than by trusting the name the
// renderer kept to itself.

#include "Checks.h"

#include "DrawTypes.h"
#include "Render.h"
#include "SectorTileset.h"
#include "TileArtwork.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorSurfaceVariant.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern UISettings gUISettings;

namespace
{
	using smoke::require;

	bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

	int const kCellWidth = CORE_CELL_WIDTH_PIXELS;
	int const kCellHeight = CORE_LEVEL_HEIGHT_PIXELS;

	// A tint no production path picks by default, so a cell that ignored the
	// caller's tint fails instead of looking right.
	pf::render::PackedColourValue const kTint = packColour(214, 96, 32, 255);

	// The missing-art colour the surface loop fills a cell with when the tile
	// it resolved to is not in the atlas.
	pf::render::PackedColourValue const kMagenta = packColour(255, 0, 255, 255);

	std::string rectText(float left, float top, float right, float bottom)
	{
		std::ostringstream out;
		out << "(" << left << "," << top << ")-(" << right << "," << bottom << ")";
		return out.str();
	}

	std::string cellName(uint32_t column, uint32_t level)
	{
		return "column " + std::to_string(column) + ", level "
			+ std::to_string(level);
	}

	// The name a cell that drew no tile at all is described by. A cell with
	// no tile behind its resolved name fills flat, and this file needs to be
	// able to say which of the two it is looking at.
	char const* const kFlat = "<flat>";

	// ------------------------------------------------------------------
	// The atlases.
	// ------------------------------------------------------------------

	// A full-cell tile per name, each in its own column of a one-row sheet,
	// so a quad's tile is readable back out of the UVs it sampled and no
	// two tiles share a texel column.
	SectorTileset installAtlas(SectorTilesetFamily family,
		std::vector<std::string> const& names)
	{
		SectorTileset atlas;
		atlas.width = int(names.size()) * kCellWidth;
		atlas.height = kCellHeight;
		atlas.family = family;
		for (size_t index = 0; index < names.size(); ++index)
			atlas.surfaces.emplace(names[index], SectorTileRegion{
				int(index) * kCellWidth, 0, kCellWidth, kCellHeight });
		require(atlas.surfaces.size() == names.size(),
			"The synthetic variant atlas dropped a duplicate tile name");
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily()
				== (family == SectorTilesetFamily::New),
			"The synthetic atlas did not come up in the family it was built as");
		return atlas;
	}

	// The set a variant World is drawn on: the Room base and two of its
	// three declared variants. `-V3` is deliberately absent, because the
	// case that matters most is the entry that outstayed the artwork.
	SectorTileset installVariantAtlas()
	{
		return installAtlas(SectorTilesetFamily::New,
			{ "room", "room-V1", "room-V2" });
	}

	// The version 1 family: a base tile and no family under any name,
	// because the old format declares none (#1038).
	SectorTileset installOldFamilyAtlas()
	{
		return installAtlas(SectorTilesetFamily::Old, { "room" });
	}

	// ------------------------------------------------------------------
	// The viewport.
	// ------------------------------------------------------------------

	// Origin and offsets pinned to zero and the zoom pinned to one, so the
	// recorded rectangles are arithmetic on the Sector's own World bounds
	// and nothing the last check left in the shared settings can move them.
	void pinViewport()
	{
		gUISettings = UISettings{};
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 2048.0f;
		gUISettings.worldViewportHeight = 2048.0f;
		gUISettings.xOffset = 0.0f;
		gUISettings.yOffset = 0.0f;
		gUISettings.worldZoom = 1.0f;
	}

	// ------------------------------------------------------------------
	// The hand-authored World.
	// ------------------------------------------------------------------

	std::string serializeWorld(core::World const& world)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	// Writes a `surfaceVariants` block into the document's Room record the
	// way a hand-authored World file carries one, and returns the document
	// text. Nothing here goes through a C++ authoring call: the ticket is
	// that a World file can say this, so this is the shape being tested.
	std::string withSurfaceVariants(std::string const& yaml,
		std::vector<core::SectorSurfaceVariant> const& entries)
	{
		// No entries means no field, which is the shape a World that never
		// used variants saves in (#1040). Writing an empty sequence would be
		// a field the feature never asked for and the loader would have to
		// have an opinion about.
		if (entries.empty()) return yaml;
		auto document = YAML::Load(yaml);
		require(document.IsMap() && document["construction"].IsSequence(),
			"The serialized World document has no construction list to author on");
		bool authored = false;
		for (auto record : document["construction"])
		{
			if (!record.IsMap()) continue;
			if (record["type"].as<std::string>() != "room") continue;
			YAML::Node variants;
			for (auto const& entry : entries)
			{
				YAML::Node item;
				item["column"] = entry.column;
				item["level"] = entry.level;
				item["variant"] = entry.variant;
				variants.push_back(item);
			}
			record["surfaceVariants"] = variants;
			authored = true;
		}
		require(authored, "The serialized World document carried no Room record");
		return YAML::Dump(document);
	}

	std::shared_ptr<const core::World> loadDocument(std::string const& yaml)
	{
		auto world = std::make_shared<core::World>("placeholder", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(world->deserialize(*reader, workData),
			"The authored World document did not load");
		return world;
	}

	// A Room built with no variants, serialized, authored with the given
	// entries, and loaded back. The returned World's Room is the one every
	// check here draws.
	struct AuthoredRoom
	{
		std::shared_ptr<const core::World> world;
		std::shared_ptr<const core::Sector> sector;
	};

	AuthoredRoom authoredRoom(uint32_t cellsWide, uint32_t levelsHigh,
		std::vector<core::SectorSurfaceVariant> const& entries)
	{
		core::World plain("Variant room world", 12, 8);
		plain.addRoom("Room", 0, 1, 1, cellsWide, levelsHigh);
		plain.finishBuild();
		plain.pauseSimulation();
		auto const world = loadDocument(withSurfaceVariants(serializeWorld(plain),
			entries));
		std::shared_ptr<const core::Sector> sector;
		for (uint32_t index = 0; index < world->getNumSectors(); ++index)
			if (world->getSector(index)->getName() == "Room") sector = world->getSector(index);
		require(sector != nullptr, "The loaded World carried no Room");
		require(sector->getCellsWide() == cellsWide
				&& sector->getLevelsHigh() == levelsHigh,
			"The loaded Room is not the size it was authored as");
		return { world, sector };
	}

	// ------------------------------------------------------------------
	// What came out of the draw.
	// ------------------------------------------------------------------

	// One surface quad as it came out of the command stream: where it
	// landed, and which tile its UVs sample.
	struct RecordedCell
	{
		std::string tile;   // empty for a cell filled with no texture at all
		float left{}, top{}, right{}, bottom{};
		// The bottom edge of the clip the cell was drawn under. The scissor
		// crops the emitted geometry here, so a cell the viewport cuts is
		// still emitted whole and this says where it stops being on screen
		// (#1048).
		float clipBottom{};
		pf::render::PackedColourValue tint{};
		bool textured{ true };
	};

	std::string describe(RecordedCell const& cell)
	{
		return (cell.textured ? cell.tile : std::string(kFlat)) + " at "
			+ rectText(cell.left, cell.top, cell.right, cell.bottom);
	}

	// The tile a sampled UV falls on, by the centre of the sampled range.
	// A crop moves the corner of that range but cannot move its centre off
	// the tile it came from, and the tiles are disjoint regions of one
	// sheet, so the centre lands on exactly one of them.
	std::string tileAt(float centreU, float centreV, SectorTileset const& atlas)
	{
		for (auto const& [name, region] : atlas.surfaces)
		{
			auto const left = float(region.x) / float(atlas.width);
			auto const right = float(region.x + region.width) / float(atlas.width);
			auto const top = float(region.y) / float(atlas.height);
			auto const bottom = float(region.y + region.height) / float(atlas.height);
			if (centreU >= left && centreU < right && centreV >= top
					&& centreV < bottom)
				return name;
		}
		return {};
	}

	// Records the Sector's surface quads out of a real `renderSector` pass.
	// Quads pair up the two triangles one AddImage call emits; a cell with
	// no tile behind it comes out as two untextured triangles in the
	// missing-art colour, which is recorded here as a cell with no tile
	// rather than skipped, because "it drew magenta" is an answer this file
	// has to be able to see.
	//
	// The clip a recorded draw runs under. Wide open by default: what these
	// cases ask is which tile each cell wears, not what the viewport leaves
	// out. A check that wants the viewport's bottom edge to cut a row's
	// interior passes one that does (#1048).
	WorldDrawList::ClipRectangle const kNoClip{ { -100000.0f, -100000.0f },
		{ 100000.0f, 100000.0f } };

	std::vector<RecordedCell> recordSurfaceCells(
		std::shared_ptr<const core::Sector> sector, uint32_t const layer,
		SectorTileset const& atlas,
		WorldDrawList::ClipRectangle const& clip = kNoClip)
	{
		WorldDrawList commands(clip);
		renderSector(sector, layer, LayerRenderStyle::Solid, false, kTint,
			&commands);

		std::vector<WorldDrawList::Triangle const*> quads;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle) continue;
			// The surface pass is the only one that draws SectorAtlas tiles or
			// the missing-art fill for them. A Room carries no other geometry,
			// and no Door, wireframe or Agent is in these fixtures.
			if (triangle->texture != WorldDrawList::Texture::SectorAtlas
					&& triangle->colour != kMagenta)
				continue;
			quads.push_back(triangle);
		}
		require(quads.size() % 2 == 0,
			"The Sector emitted an odd number of surface triangles, so they are "
			"not the pairs one quad makes");

		std::vector<RecordedCell> cells;
		for (size_t pair = 0; pair < quads.size(); pair += 2)
		{
			auto const& first = *quads[pair];
			auto const& second = *quads[pair + 1];
			require(first.colour == second.colour,
				"The two triangles of one surface quad were tinted differently");
			RecordedCell cell;
			cell.tint = first.colour;
			cell.clipBottom = first.clip.maximum.y;
			require(near(second.clip.maximum.y, cell.clipBottom),
				"The two triangles of one surface quad were recorded under "
				"different clips");
			cell.left = cell.top = 1e9f;
			cell.right = cell.bottom = -1e9f;
			float uvLeft = 1e9f, uvRight = -1e9f, uvTop = 1e9f, uvBottom = -1e9f;
			for (auto const* triangle : { &first, &second })
			{
				for (auto const& p : triangle->positions)
				{
					cell.left = std::min(cell.left, p.x);
					cell.right = std::max(cell.right, p.x);
					cell.top = std::min(cell.top, p.y);
					cell.bottom = std::max(cell.bottom, p.y);
				}
				for (auto const& uv : triangle->texcoords)
				{
					uvLeft = std::min(uvLeft, uv.x);
					uvRight = std::max(uvRight, uv.x);
					uvTop = std::min(uvTop, uv.y);
					uvBottom = std::max(uvBottom, uv.y);
				}
			}
			if (first.texture == WorldDrawList::Texture::SectorAtlas)
			{
				cell.textured = true;
				cell.tile = tileAt((uvLeft + uvRight) * 0.5f,
					(uvTop + uvBottom) * 0.5f, atlas);
				require(!cell.tile.empty(),
					"A textured surface quad sampled UVs that fall on no tile in "
					"the installed atlas");
			}
			else
			{
				cell.textured = false;
				require(cell.tint == kMagenta,
					"An untextured surface quad carried a colour other than the "
					"missing-art magenta");
			}
			cells.push_back(cell);
		}
		return cells;
	}

	// The tile the rule says one cell draws, given the entries and what the
	// installed atlas carries. Stated in the check rather than read out of
	// the renderer, so a renderer that fell back wrongly fails against a
	// rule that says otherwise. A resolved name the atlas does not carry -
	// the base itself, on a sheet built without one - answers as a flat
	// fill, because that is what the missing-art case looks like.
	std::string expectedTile(SectorTileset const& atlas,
		std::vector<core::SectorSurfaceVariant> const& entries,
		uint32_t column, uint32_t level)
	{
		std::string resolved = "room";
		for (auto const& entry : entries)
		{
			if (entry.column != column || entry.level != level) continue;
			auto const variant = "room-V" + std::to_string(entry.variant);
			if (atlas.surfaces.count(variant)) resolved = variant;
			break;
		}
		return atlas.surfaces.count(resolved) ? resolved : std::string(kFlat);
	}

	// Pairs each expected cell with the recorded cell that landed on it.
	// The contract is on what is drawn, not on the order it was emitted.
	std::vector<RecordedCell> matchedToGrid(
		std::shared_ptr<const core::Sector> sector,
		std::vector<RecordedCell> const& recorded,
		std::vector<core::SectorSurfaceVariant> const& entries,
		SectorTileset const& atlas)
	{
		auto const cellsWide = sector->getCellsWide();
		auto const levelsHigh = sector->getLevelsHigh();
		require(recorded.size() == size_t(cellsWide) * size_t(levelsHigh),
			"The Sector drew " + std::to_string(recorded.size())
				+ " surface cells; its grid is " + std::to_string(cellsWide)
				+ " by " + std::to_string(levelsHigh));

		// The Room's own screen area, from its World bounds at the pinned
		// viewport: x scales by the cell width, y by the Level height and
		// flips, and the viewport offsets are added.
		core::Vector2 low{}, high{};
		sector->getBounds(low, high);
		auto const toScreen = [](core::Vector2 const& point)
		{
			return pf::render::Vec2{
				point.x * CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom
					+ gUISettings.worldViewportX + gUISettings.xOffset,
				gUISettings.worldViewportY + gUISettings.worldViewportHeight
					- point.y * CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom
					- gUISettings.yOffset };
		};
		auto const a = toScreen(low);
		auto const b = toScreen(high);
		float const left = std::min(a.x, b.x);
		float const floorLine = std::max(a.y, b.y);
		float const cellWidth = CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom;
		float const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom;

		std::vector<RecordedCell> matched;
		std::vector<bool> taken(recorded.size(), false);
		for (uint32_t level = 0; level < levelsHigh; ++level)
		{
			float const bottom = floorLine - float(level) * cellHeight;
			float const top = std::max(bottom - cellHeight, std::min(a.y, b.y));
			for (uint32_t column = 0; column < cellsWide; ++column)
			{
				float const cellLeft = left + float(column) * cellWidth;
				size_t found = recorded.size();
				for (size_t index = 0; index < recorded.size(); ++index)
				{
					if (taken[index]) continue;
					auto const& cell = recorded[index];
					if (near(cell.left, cellLeft) && near(cell.right, cellLeft + cellWidth)
							&& near(cell.top, top) && near(cell.bottom, bottom))
					{
						found = index;
						break;
					}
				}
				require(found != recorded.size(),
					"Nothing was drawn at " + cellName(column, level) + " ("
						+ rectText(cellLeft, top, cellLeft + cellWidth, bottom) + ")");
				taken[found] = true;
				matched.push_back(recorded[found]);
				auto const want = expectedTile(atlas, entries, column, level);
				auto const& got = recorded[found];
				auto const gotTile = got.textured ? got.tile : std::string(kFlat);
				require(gotTile == want,
					"The cell at " + cellName(column, level) + " drew '" + gotTile
						+ "'; the rule says '" + want + "'");
				// A textured cell carries the tint the caller asked for. A cell
				// with no tile behind it is the missing-art fill, which is its own
				// colour by design and not the caller's.
				require(got.textured ? got.tint == kTint : got.tint == kMagenta,
					"The cell at " + cellName(column, level) + " carried " + std::to_string(got.tint)
						+ "; a textured cell carries the tint asked for and an "
						"untextured one carries the missing-art magenta");
			}
		}
		for (size_t index = 0; index < recorded.size(); ++index)
			require(taken[index],
				"The Sector drew a surface cell at " + describe(recorded[index])
					+ " that is not on its grid");
		return matched;
	}

	// ------------------------------------------------------------------
	// The cases.
	// ------------------------------------------------------------------

	// Entries that cover each of the three answers a recorded cell can get:
	// a variant the atlas has, a variant the atlas does not have, and - by
	// being absent - the base everywhere else.
	std::vector<core::SectorSurfaceVariant> mixedEntries()
	{
		return { { 0, 0, 1 }, { 2, 1, 2 }, { 3, 0, 3 } };
	}

	// A recorded cell draws its variant tile; every other cell draws the
	// base. Nothing else is drawn and nothing is left undrawn.
	void aRecordedCellDrawsItsVariantTileAndEveryOtherCellDrawsBase(
		smoke::Context const&)
	{
		pinViewport();
		auto const atlas = installVariantAtlas();
		auto const entries = mixedEntries();
		auto const fixture = authoredRoom(4, 2, entries);
		require(fixture.sector->getSurfaceVariants().size() == entries.size(),
			"The loaded Sector did not carry the entries the document recorded");
		require(fixture.sector->surfaceVariantAt(0, 0)
				== std::optional<uint32_t>{ 1 },
			"The loaded Sector does not answer for the cell at column 0, level 0");
		require(fixture.sector->surfaceVariantAt(2, 1)
				== std::optional<uint32_t>{ 2 },
			"The loaded Sector does not answer for the cell at column 2, level 1");
		require(!fixture.sector->surfaceVariantAt(1, 0),
			"A cell with no entry answered as though it had one");
		auto const recorded = recordSurfaceCells(fixture.sector,
			fixture.sector->getLayerIndex(), atlas);
		matchedToGrid(fixture.sector, recorded, entries, atlas);

		// Stated once more, plainly: the two cells with tiles behind them
		// came off their own variant regions, and no cell came off a variant
		// region it was not recorded for.
		int variantCells = 0;
		for (auto const& cell : recorded)
			if (cell.tile != "room") ++variantCells;
		require(variantCells == 2,
			"Expected exactly two cells to leave the base tile region, found "
				+ std::to_string(variantCells));
	}

	// A recorded variant whose tile the installed atlas does not carry
	// draws the base and does not complain. `-V3` is not on this sheet, so
	// the cell recorded as wearing it is drawn as plain `room` among its
	// neighbours, and nothing anywhere in the stream says otherwise.
	void aVariantTheAtlasDoesNotCarryFallsBackToBaseSilently(
		smoke::Context const&)
	{
		pinViewport();
		auto const atlas = installVariantAtlas();
		require(!atlas.surfaces.count("room-V3"),
			"The test atlas was supposed to leave -V3 out");
		require(!hasSectorTileSurface("room-V3"),
			"The installed atlas reports carrying a tile it was not built with");
		auto const entries = std::vector<core::SectorSurfaceVariant>{ { 1, 0, 3 } };
		auto const fixture = authoredRoom(3, 1, entries);
		// The entry is kept. Falling back in the picture is not the same act
		// as forgetting the author wrote it: the tile may arrive with the
		// next sheet, and the entry has to still be there when it does.
		require(fixture.sector->getSurfaceVariants().size() == 1,
			"A variant with no tile behind it was not retained on the Sector");
		auto const recorded = recordSurfaceCells(fixture.sector,
			fixture.sector->getLayerIndex(), atlas);
		matchedToGrid(fixture.sector, recorded, entries, atlas);
		for (auto const& cell : recorded)
			require(cell.tile == "room",
				"A cell drew " + describe(cell) + "; an unbacked variant falls "
				"back to the base tile and nothing else");
	}

	// A missing base tile still draws the missing-art magenta. The variant
	// that has its own art still wears it; what falls back to a base that is
	// not there is magenta, cell by cell, which is what the whole-area
	// magenta looked like when the whole area was one tile.
	void aMissingBaseTileStillDrawsTheMissingArtMagenta(smoke::Context const&)
	{
		pinViewport();
		// `-V1` and nothing else: no base tile at all behind this World.
		auto const atlas = installAtlas(SectorTilesetFamily::New, { "room-V1" });
		require(!hasSectorTileSurface("room"),
			"The test atlas was supposed to leave the base tile out");
		auto const entries = std::vector<core::SectorSurfaceVariant>{
			{ 0, 0, 1 }, { 1, 1, 3 } };
		auto const fixture = authoredRoom(2, 2, entries);
		auto const recorded = recordSurfaceCells(fixture.sector,
			fixture.sector->getLayerIndex(), atlas);
		matchedToGrid(fixture.sector, recorded, entries, atlas);

		int magenta = 0;
		for (auto const& cell : recorded)
		{
			if (cell.textured)
			{
				require(cell.tile == "room-V1",
					"The only cell with art behind it drew " + cell.tile);
				continue;
			}
			++magenta;
			require(cell.tint == kMagenta,
				"A cell with no tile behind it drew a colour other than the "
				"missing-art magenta");
		}
		require(magenta == 3,
			"Expected three cells to fall back to a base that is not there and "
			"draw magenta; found " + std::to_string(magenta));
	}

	// A World repointed at the version 1 family draws base on every
	// variant cell and keeps its entries, so repointing it back restores
	// the picture with nothing rewritten. The old family declares no
	// variant families at all (#1038), so no `-Vn` name resolves, and the
	// fallback rule answers base for every cell - which is what this World
	// drew before variants existed and is what it must draw again.
	void theOldFamilyDrawsBaseOnEveryVariantCellAndKeepsTheEntries(
		smoke::Context const&)
	{
		pinViewport();
		auto const entries = mixedEntries();
		core::World plain("Variant room world", 12, 8);
		plain.addRoom("Room", 0, 1, 1, 4, 2);
		plain.finishBuild();
		plain.pauseSimulation();
		auto const document = withSurfaceVariants(serializeWorld(plain), entries);

		auto const oldAtlas = installOldFamilyAtlas();
		require(!hasSectorTileSurface("room-V1"),
			"The version 1 family atlas was supposed to carry no variant tiles");
		auto const onOldFamily = loadDocument(document);
		std::shared_ptr<const core::Sector> oldSector;
		for (uint32_t index = 0; index < onOldFamily->getNumSectors(); ++index)
			if (onOldFamily->getSector(index)->getName() == "Room")
				oldSector = onOldFamily->getSector(index);
		require(oldSector != nullptr, "The repointed World lost its Room");
		require(oldSector->getSurfaceVariants().size() == entries.size(),
			"Repointing the World at the old family lost its variant entries; "
			"the picture changes, the author's data does not");
		auto const recorded = recordSurfaceCells(oldSector,
			oldSector->getLayerIndex(), oldAtlas);
		matchedToGrid(oldSector, recorded, entries, oldAtlas);
		for (auto const& cell : recorded)
			require(cell.textured && cell.tile == "room",
				"A variant cell drew " + describe(cell) + " on the version 1 "
				"family; it draws the base");

		// And back the other way: the same document, the same entries, on a
		// family that has the tiles, draws the variants it could not draw a
		// moment ago. Nothing was rewritten between the two loads.
		auto const newAtlas = installVariantAtlas();
		auto const backAgain = loadDocument(document);
		std::shared_ptr<const core::Sector> restored;
		for (uint32_t index = 0; index < backAgain->getNumSectors(); ++index)
			if (backAgain->getSector(index)->getName() == "Room")
				restored = backAgain->getSector(index);
		require(restored->getSurfaceVariants() == oldSector->getSurfaceVariants(),
			"The entries changed between the old-family load and the new one");
		auto const restoredCells = recordSurfaceCells(restored,
			restored->getLayerIndex(), newAtlas);
		matchedToGrid(restored, restoredCells, entries, newAtlas);
		int variantCells = 0;
		for (auto const& cell : restoredCells)
			if (cell.tile != "room") ++variantCells;
		require(variantCells == 2,
			"Repointing back at a family with the tiles restored "
				+ std::to_string(variantCells) + " variant cells; two were asked for");
	}

	// A variant is a different picture and not a different geometry. The
	// same Room, drawn twice - once with no entries and once with them -
	// emits the same cells at the same rectangles with the same crop; only
	// the tile each cell samples differs. This is the #1037 contract seen
	// from the other side: a variant must not re-anchor, resize or
	// re-crop anything.
	void aVariantChangesThePictureAndNotTheGeometry(smoke::Context const&)
	{
		pinViewport();
		auto const atlas = installVariantAtlas();

		auto const plain = authoredRoom(4, 3, {});
		require(plain.sector->getSurfaceVariants().empty(),
			"The no-variant fixture carried entries");
		auto const plainCells = recordSurfaceCells(plain.sector,
			plain.sector->getLayerIndex(), atlas);

		auto const varied = authoredRoom(4, 3, mixedEntries());
		auto const variedCells = recordSurfaceCells(varied.sector,
			varied.sector->getLayerIndex(), atlas);

		require(plainCells.size() == variedCells.size(),
			"Recording variants changed how many cells the surface drew");
		std::vector<bool> taken(variedCells.size(), false);
		int differing = 0;
		for (auto const& base : plainCells)
		{
			size_t found = variedCells.size();
			for (size_t index = 0; index < variedCells.size(); ++index)
			{
				if (taken[index]) continue;
				auto const& cell = variedCells[index];
				if (near(cell.left, base.left) && near(cell.right, base.right)
						&& near(cell.top, base.top)
						&& near(cell.bottom, base.bottom))
				{
					found = index;
					break;
				}
			}
			require(found != variedCells.size(),
				"The variant World drew nothing at the rectangle a no-variant "
				"cell occupied: " + rectText(base.left, base.top, base.right,
					base.bottom));
			taken[found] = true;
			auto const& cell = variedCells[found];
			require(cell.textured == base.textured
					&& cell.tint == base.tint,
				"A variant cell changed the fill or the tint as well as the tile");
			if (cell.tile != base.tile) ++differing;
		}
		require(differing == 2,
			"Exactly two cells should differ from the no-variant picture; found "
				+ std::to_string(differing) + " (the third recorded variant has "
				"no tile behind it and falls back to base)");
	}

	// The screen y of a Sector's floor line at the pinned viewport. World y
	// runs up and screen y runs down, so the floor - the low end of the
	// Sector's World bounds - is the larger screen y.
	float screenFloorLine(core::Sector const& sector)
	{
		core::Vector2 low{}, high{};
		sector.getBounds(low, high);
		auto const toScreenY = [](float worldY)
		{
			return gUISettings.worldViewportY + gUISettings.worldViewportHeight
				- worldY * CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom
				- gUISettings.yOffset;
		};
		return std::max(toScreenY(low.y), toScreenY(high.y));
	}

	// The viewport's bottom edge cutting through a row does not take the
	// artwork with it (#1048).
	//
	// Scroll a World until a Room's floor line sits below the bottom of the
	// viewport, and the lowest visible row of that Room's surface is a row
	// the clip cuts through. The loop counts rows up from the floor line,
	// and until #1048 it rounded the distance from the clip's bottom edge
	// to that line up: a clip one pixel above the floor rounded to a whole
	// cell, and the bottom row - 319 of its 320 pixels on screen - was
	// never emitted. Once a cell could wear a variant the author painted
	// (#1040) that stopped being a strip of missing background and became
	// lost artwork that no test could see.
	//
	// Driven the way the ticket describes it: a Room with variants recorded
	// on its bottom row, drawn through the real surface path under a clip
	// that cuts that row. Every cell still draws, at its full World cell,
	// carrying the clip that crops it.
	void aVariantOnTheRowTheViewportBottomCutsStillDraws(smoke::Context const&)
	{
		using smoke::require;
		pinViewport();
		auto const atlas = installVariantAtlas();
		auto const entries = std::vector<core::SectorSurfaceVariant>{
			{ 0, 0, 1 }, { 1, 0, 2 } };
		auto const fixture = authoredRoom(2, 3, entries);
		float const cellHeight = CORE_LEVEL_HEIGHT_PIXELS
			* gUISettings.worldZoom;
		float const floorLine = screenFloorLine(*fixture.sector);
		float const clipBottom = floorLine - 1.0f;
		auto const recorded = recordSurfaceCells(fixture.sector,
			fixture.sector->getLayerIndex(), atlas,
			{ { -100000.0f, -100000.0f }, { 100000.0f, clipBottom } });

		// Nothing was counted out of the grid: all six cells drew, each the
		// tile the rule says it wears.
		require(recorded.size() == 6,
			"The viewport bottom cut left " + std::to_string(recorded.size())
				+ " surface cells; the Room is 2 by 3 and every one of them is at "
				"least partly above the clip");
		matchedToGrid(fixture.sector, recorded, entries, atlas);

		int straddling = 0;
		for (auto const& cell : recorded)
		{
			require(near(cell.clipBottom, clipBottom),
				"A surface cell did not carry the clip that crops it: "
					+ describe(cell));
			auto const level = std::lround((floorLine - cell.bottom)
				/ cellHeight);
			if (level != 0) continue;
			++straddling;
			// The row the cut lands in is emitted whole: its own bottom edge
			// on the floor line, one full cell tall, with the scissor taking
			// the pixel below the viewport. Shrinking the rectangle to the
			// clip instead would stretch the tile, which is the other way to
			// get this wrong and is what this assertion is for.
			require(near(cell.bottom, floorLine)
					&& near(cell.top, floorLine - cellHeight)
					&& cell.textured && cell.tile != "room",
				"The variant cell on the row the viewport bottom cuts drew "
					+ describe(cell) + "; it draws whole at its own World cell "
					"and the clip crops it, which keeps one artwork pixel to one "
					"screen pixel");
		}
		require(straddling == 2,
			"Found " + std::to_string(straddling)
				+ " cells on the row the viewport bottom cuts; both variant cells "
				"of the bottom row were asked for");
	}
}

void render_smoke::registerSectorSurfaceVariants(std::vector<smoke::Check>& checks)
{
	checks.push_back(
		{ "sectorSurfaceVariants/aRecordedCellDrawsItsVariantTileAndEveryOtherCellDrawsBase",
			isolated<aRecordedCellDrawsItsVariantTileAndEveryOtherCellDrawsBase> });
	checks.push_back(
		{ "sectorSurfaceVariants/aVariantTheAtlasDoesNotCarryFallsBackToBaseSilently",
			isolated<aVariantTheAtlasDoesNotCarryFallsBackToBaseSilently> });
	checks.push_back(
		{ "sectorSurfaceVariants/aMissingBaseTileStillDrawsTheMissingArtMagenta",
			isolated<aMissingBaseTileStillDrawsTheMissingArtMagenta> });
	checks.push_back(
		{ "sectorSurfaceVariants/theOldFamilyDrawsBaseOnEveryVariantCellAndKeepsTheEntries",
			isolated<theOldFamilyDrawsBaseOnEveryVariantCellAndKeepsTheEntries> });
	checks.push_back(
		{ "sectorSurfaceVariants/aVariantChangesThePictureAndNotTheGeometry",
			isolated<aVariantChangesThePictureAndNotTheGeometry> });
	checks.push_back(
		{ "sectorSurfaceVariants/aVariantOnTheRowTheViewportBottomCutsStillDraws",
			isolated<aVariantOnTheRowTheViewportBottomCutsStillDraws> });
}
