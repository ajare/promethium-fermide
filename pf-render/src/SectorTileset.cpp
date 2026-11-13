#include "SectorTileset.h"
#include "AtlasInstall.h"
#include "AuthoredTileGrid.h"
#include "core/Defines.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
// Whether the handle a caller passed names a sheet there is anything to
// draw from: a sheet, with pixels behind it. A null handle is no sheet at
// all, and a sheet installed behind an empty handle has no pixels to draw,
// so it answers every question the way no sheet does - which is what
// makes a missing atlas draw magenta instead of drawing nothing (#1003).
//
// This is the whole of what these helpers used to reach for in file state,
// reduced to a predicate on the argument in front of it. Which sheet a draw
// path draws is the caller's answer, and no helper here goes looking for it
// (#1088).
bool sheetHasArtwork(pf::render::InstalledSectorSheet const* sheet)
{
    return sheet != nullptr && sheet->installed();
}

// The twelve surface-bearing Sector types. Each fills its own full-cell
// tile. Background and Facade are in the set on the same terms as every
// other type: their surface is artwork the sheet carries, not a colour the
// renderer mixes, so they ask for a tile like any wall does.
constexpr std::array<char const*, 12> kSectorSurfaces{ "room", "corridor",
    "ladder", "lift", "shuttle", "stairwell", "staircase", "airlock",
    "chamber", "dumbwaiter", "background", "facade" };

// A version 2 sector tileset authors the same grid the object atlases use:
// a cell in the artist's own layout, a content rectangle inside it, and
// the render semantics per tile (#1014). What differs is what the set is
// for. Every tile in it is a surface; the boundary strips are dropped, so
// every Sector edge falls back to the primitive line the renderer already
// emits. A file that still declares a `boundaries` map is refused rather
// than quietly ignored, which is what stops the dropped artwork coming
// back by accident.
//
// A surface is one full World cell of artwork repeated once per cell across
// the Sector's area. A tile drawn at any other size, or offset off its
// cell's anchor, would repeat at a geometry the per-cell repeat rule cannot
// express, so the loader refuses it rather than drawing something that
// contradicts its own repeat.
//
// A variant family is derived by the shared grid rather than here. Each base
// sprite's `variants: N` yields `-V1` .. `-VN` in the base's own column,
// one row below the last, each inheriting the base's content rectangle,
// render offset and drawn size. The author states the count
// and the grid does the rest, so a variant's name and its position cannot
// disagree with the family they belong to - and the object loader derives
// the identical rule from the identical code rather than restating it
// (#1038, #1053).
SectorTileset loadVersion2SectorTileset(std::filesystem::path const& path,
    YAML::Node const& root)
{
    if (root["boundaries"].IsDefined())
        throw std::runtime_error("A version 2 sector tileset declares no boundary "
            "strips; every Sector edge draws the primitive line ('"
            + path.filename().string() + "')");
    // `variants` is a per-sprite key. Written on the file it would read as a
    // set-wide count with no base to descend from, which is not what the key
    // means, so it is refused rather than given a meaning nobody wrote.
    if (root["variants"].IsDefined())
        throw std::runtime_error("`variants` is declared on a base sprite, not on "
            "the file; each family descends from the sprite that stated it ('"
            + path.filename().string() + "')");
    SectorTileset result;
    result.image = path.parent_path() / root["image"].as<std::string>();
    result.family = SectorTilesetFamily::New;
    // A Sector surface and a doorframe are the same shape of thing - one
    // full cell at the cell's anchor, drawn once per World cell - so the
    // same two refusals govern both. Saying it once keeps a frame from
    // being let through with a geometry the per-cell repeat rule cannot
    // express just because it arrived through the other map.
    auto const requireFullCell = [](std::string const& name,
        std::string const& what, TileArtwork const& artwork)
    {
        if (artwork.offset.x != 0.0f || artwork.offset.y != 0.0f)
            throw std::runtime_error(what + " '" + name
                + "' must be drawn at its cell's anchor, because the repeat "
                "is per World cell");
        if (artwork.drawnSize.x != 1.0f || artwork.drawnSize.y != 1.0f)
            throw std::runtime_error(what + " '" + name
                + "' must be drawn at one full World cell, because the "
                "repeat is per World cell");
    };
    auto const grid = loadAuthoredTileGrid(root, true, "sector surface",
        AuthoredVariantFamilies::Derived);
    result.width = grid.width;
    result.height = grid.height;
    // The grid hands back the base sprites and their derived variants in one
    // map, and every one of them is a surface: this format has no other kind
    // of tile in `sprites`. A variant inherits its base's offset and drawn
    // size, so the two checks below that a base passes its variants pass for
    // the same reason, and a variant that somehow did not inherit them is
    // refused here rather than drawn off the per-cell repeat rule.
    for (auto const& [name, artwork] : grid.tiles) {
        requireFullCell(name, "Sector surface", artwork);
        result.tiles.emplace(name, artwork);
        result.surfaces.emplace(name, artwork.region);
    }
    // The overlays are read through the same grid rule as the surfaces,
    // because they are laid out on the same sheet the same way: a cell,
    // a content rectangle inside it, and the render semantics the tile
    // draws with. What the second map buys is a second answer to "what
    // kind of tile is this", which is a question the sheet itself cannot
    // ask (#1054). A file with no `overlays` map is a file with no
    // overlays - every version 2 set written before the doorframe existed
    // reads this way, and draws its door cells as its wall's plain tile,
    // exactly as it always did.
    //
    // Guarded on the shape rather than on the key's presence alone, because
    // in yaml-cpp "absent" and "written with no body" are two different
    // node states that both mean no overlays: a missing key is a zombie node
    // that `IsDefined()` reports as absent, and `overlays:` with no body is
    // a *defined Null*. Guarding on `IsDefined()` alone therefore read the
    // second - the map an author stubs before filling it in, or strips back
    // during art iteration - as a file that declared overlays and then fell
    // over the grid's own map check, while the generator read the identical
    // file as no overlays at all and declared it clean. The refusal landed
    // after `--check` had already passed, and read as though the author had
    // never written the map (#1066).
    //
    // `IsDefined() && !IsNull()` covers both spellings the way the
    // generator's `overlays is None` does, and leaves anything that is not a
    // map - a scalar, a sequence - to fall through to the grid, which
    // refuses it, so the two readers agree in both directions. `IsNull()`
    // on its own is not usable here: it throws on the zombie an absent key
    // hands back, which is the one shape the guard also has to swallow.
    auto const overlays = root["overlays"];
    if (overlays.IsDefined() && !overlays.IsNull())
    {
        // The overlays derive nothing, and the `-Vn` shape is still the
        // derivation's, because the derivation exists - in this same
        // file's `sprites` map. `variantFamilies` says only that this map
        // grows no family column; `reserveVariantNames` is asked
        // separately, so an overlay authored as `doorframe-room-1r-L0-V1`
        // is refused rather than reading as a family nothing made, and a
        // family declared later cannot be stopped by a name an author had
        // already written (#1069).
        auto const overlayGrid = loadAuthoredTileGrid(root, true,
            "sector overlay", AuthoredVariantFamilies::Refused, "overlays",
            true);
        if (overlayGrid.width != grid.width || overlayGrid.height != grid.height)
            throw std::runtime_error("A sector tileset's overlays are authored "
                "on the same sheet as its surfaces ('" + path.filename().string()
                + "')");
        for (auto const& [name, artwork] : overlayGrid.tiles) {
            requireFullCell(name, "Sector overlay", artwork);
            // One name, two kinds. Which map the draw would have found it in
            // would come down to lookup order, and a frame drawing as a wall
            // - or a wall drawing as a frame - is not a thing two maps
            // should be left to settle between themselves.
            if (result.surfaces.count(name) > 0)
                throw std::runtime_error("Sector overlay '" + name
                    + "' is also authored as a Sector surface; a tile is one "
                    "kind or the other");
            // The install routes a `boundary-` prefix to the boundary map
            // before it looks at anything else, so an overlay wearing that
            // prefix would be installed as edge artwork nobody asked for.
            if (name.rfind("boundary-", 0) == 0)
                throw std::runtime_error("Sector overlay '" + name
                    + "' takes the `boundary-` prefix, which the install "
                    "routes to boundary artwork");
            result.tiles.emplace(name, artwork);
            result.overlays.emplace(name, artwork.region);
        }
    }
    // The count is the author's own number, kept beside the base that stated
    // it rather than counted back off the derived tiles, so a caller can ask
    // how many variants a type declared (#1038).
    result.variantCounts = grid.variantCounts;
    for (auto const* name : kSectorSurfaces)
        if (!result.surfaces.count(name))
            throw std::runtime_error("Missing sector surface: " + std::string(name));
    return result;
}

// Draws one strip of a tile region into a screen rectangle. The four
// fractions say which part of the tile reaches the screen: `u0`/`u1` run
// left to right across the tile, `v0`/`v1` top to bottom. A whole tile is
// 0..1 on both axes; a crop is a sub-range drawn at the same pixels per
// artwork texel the whole tile would have had, which is what keeps a crop a
// crop and not a squash.
//
// The sheet is the argument rather than the tileset because the recording
// needs both: the region's fractions are divided by the sheet's own size,
// and the triangle has to say which sheet it came from so the batch that
// draws it binds that sheet's texture and not the one installed last
// (#1089). Taking the sheet whole is what makes it impossible to divide a
// region by one sheet's dimensions and record another sheet's name beside
// the result.
void image(WorldDrawList* list, pf::render::InstalledSectorSheet const& sheet,
    SectorTileRegion const& r, Vec2 a, Vec2 b,
    float u0, float u1, float v0, float v1)
{
    auto const& tileset = sheet.tileset;
    // Half-texel inset avoids sampling adjacent draft artwork with linear
    // filtering. The divisor is the width and height of the sheet this
    // region was taken out of, which is the sheet the caller handed the
    // draw: a region's fractions are only meaningful against the sheet
    // they were cut from, so dividing them by some other sheet's size is
    // how a tile would sample across the middle of a picture it has
    // nothing to do with (#1088).
    Vec2 uv0{(r.x + 0.5f + (r.width - 1) * u0) / tileset.width,
        (r.y + 0.5f + (r.height - 1) * v0) / tileset.height};
    Vec2 uv1{(r.x + 0.5f + (r.width - 1) * u1) / tileset.width,
        (r.y + 0.5f + (r.height - 1) * v1) / tileset.height};
    // White on every draw: the tile's own pixels are the picture, and no
    // colour is multiplied into it on the way to the screen.
    list->AddSectorImage(sheet.name, a, b, uv0, uv1, White);
}

// How many whole World cells a span covers, counted from where it starts.
// The tolerance is not slack. The screen coordinates the span was built
// from carry float round-off of their own, so a Sector four cells wide can
// read as 4.0000004 cells across, and counted without the tolerance that
// fraction becomes a fifth column a hundredth of a pixel wide. A repeat
// that walked the area could hide such a sliver; a loop that resolves a
// tile per cell cannot, because the phantom cell is a cell it has to
// answer for.
int cellsCovered(float span, float cell)
{
    if (cell <= 0.0f || span <= 0.0f) return 0;
    return std::max(0, static_cast<int>(std::ceil(span / cell - 1e-4f)));
}

// Puts one resolved cell on the screen. A name the installed atlas does
// not carry is the missing-tile magenta, and it is magenta for that cell
// alone: the resolver owns the question of which tile, so it owns what a
// wrong answer looks like. A Sector whose base tile is genuinely absent
// still fills its whole area with magenta, one cell at a time, which is
// what it looked like when the whole area was filled at once.
void drawSurfaceCell(WorldDrawList* list,
    pf::render::InstalledSectorSheet const& sheet,
    std::string const& kind, Vec2 a,
    Vec2 b, float u0, float u1, float v0, float v1)
{
    auto const& tileset = sheet.tileset;
    // A resolved name is looked for among the surfaces first, then among
    // the overlays. The resolver owns which name it answers with and the
    // overlay map is how it says "this cell wears a doorframe"; the two
    // maps stay apart so the queries about them stay apart, but the draw
    // path is one loop and a frame has to be drawable through it without
    // a second loop beside it that would have to relearn the anchor, the
    // crop and the culling (#1054).
    //
    // Held as a pointer rather than as an iterator carried between the two
    // lookups: an iterator into `surfaces` and an iterator into `overlays`
    // are the same type and must never be compared to each other, which is
    // undefined behaviour and one of the few ways this function could be
    // wrong without any test being able to see it.
    SectorTileRegion const* region = nullptr;
    if (auto const surface = tileset.surfaces.find(kind);
        surface != tileset.surfaces.end())
        region = &surface->second;
    else if (auto const overlay = tileset.overlays.find(kind);
        overlay != tileset.overlays.end())
        region = &overlay->second;
    if (!region || tileset.width <= 0 || tileset.height <= 0) {
        list->AddRectFilled(a, b, packColour(255, 0, 255, 255));
        return;
    }
    image(list, sheet, *region, a, b, u0, u1, v0, v1);
}
}

SectorTileset SectorTileset::load(std::filesystem::path const& path)
{
    auto root = YAML::LoadFile(path.string());
    int const version = root["version"].as<int>();
    // Version 1 is the draft's irregular-rectangle format: explicit regions
    // for seven surfaces and four boundary strips. Version 2 is the authored
    // grid, carries no boundary artwork at all, and names every one of the
    // ten surface-bearing Sector types (#1017).
    if (version == 2) return loadVersion2SectorTileset(path, root);
    if (version != 1)
        throw std::runtime_error("Unsupported sector tileset version");
    // The version 1 format has no variant families: its surfaces are bare
    // rectangles with no cell to descend from, so a `variants` key here is a
    // family nothing derives. Refused rather than ignored, because an ignored
    // declaration reads as one that worked (#1038).
    if (root["variants"].IsDefined())
        throw std::runtime_error("A version 1 sector tileset has no variant families; "
            "`variants` is a version 2 base-sprite key ('" + path.filename().string()
            + "')");
    SectorTileset result;
    result.image = path.parent_path() / root["image"].as<std::string>();
    // The draft family. Stated rather than left to the default so the two
    // loaders each say which family they produce (#1027).
    result.family = SectorTilesetFamily::Old;
    result.width = root["size"][0].as<int>();
    result.height = root["size"][1].as<int>();
    if (result.width <= 0 || result.height <= 0)
        throw std::runtime_error("Invalid sector atlas dimensions");
    auto read = [&](char const* key, auto& destination) {
        if (!root[key].IsMap()) throw std::runtime_error("Missing tileset region map");
        for (auto const& entry : root[key]) {
            auto const v = entry.second;
            if (!v.IsSequence() || v.size() != 4)
                throw std::runtime_error("Tileset regions require [x, y, width, height]");
            SectorTileRegion r{v[0].as<int>(), v[1].as<int>(), v[2].as<int>(), v[3].as<int>()};
            if (r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0 ||
                r.width > result.width || r.height > result.height ||
                r.x > result.width - r.width || r.y > result.height - r.height)
                throw std::runtime_error("Sector atlas region is out of bounds");
            destination.emplace(entry.first.as<std::string>(), r);
        }
    };
    read("surfaces", result.surfaces);
    read("boundaries", result.boundaries);
    for (auto const& [name, region] : result.surfaces)
        result.tiles.emplace(name, TileArtwork{ region, { 0.0f, 0.0f }, { 1.0f, 1.0f } });
    for (auto const& [name, region] : result.boundaries)
        result.tiles.emplace("boundary-" + name,
            TileArtwork{ region, { 0.0f, 0.0f }, { 1.0f, 1.0f } });
    for (auto name : {"corridor", "room", "ladder", "lift", "shuttle", "stairwell", "staircase"})
        if (!result.surfaces.count(name)) throw std::runtime_error("Missing sector surface: " + std::string(name));
    for (auto name : {"floor", "ceiling", "left", "right"})
        if (!result.boundaries.count(name)) throw std::runtime_error("Missing sector boundary: " + std::string(name));
    return result;
}

void setSectorTileset(SectorTileset tileset, TextureId texture)
{
    // The nameless install: the default Sector sheet, installed under the
    // default name and made the sheet the draw path draws. Every caller of
    // this is standing in for the World's own Sector artwork; the install
    // the application runs names its sheet explicitly (#1087).
    pf::render::sectorSheetRegistry().install(
        pf::render::kDefaultSectorAtlasName, std::move(tileset), texture);
}
void clearSectorTileset() { pf::render::sectorSheetRegistry().clear(); }

bool hasSectorTileSurface(pf::render::InstalledSectorSheet const* sheet,
    std::string const& kind)
{
    return sheetHasArtwork(sheet) && sheet->tileset.surfaces.count(kind) > 0;
}

bool hasSectorTileOverlay(pf::render::InstalledSectorSheet const* sheet,
    std::string const& name)
{
    // Guarded on the sheet the same way the surface query is: with no
    // pixels behind the handle there is no frame to draw, and a caller that
    // asks gets the answer that makes it fall back rather than the answer
    // that makes it draw magenta.
    return sheetHasArtwork(sheet) && sheet->tileset.overlays.count(name) > 0;
}

bool sectorTilesetIsNewFamily(pf::render::InstalledSectorSheet const* sheet)
{
    // A sheet with nothing installed behind it is not the new family. The
    // handle decides which sheet the question was even asked of, so a
    // teardown that drops the installed sheets cannot leave a stale promise
    // that own art is available standing under some other sheet's name.
    return sheetHasArtwork(sheet)
        && sheet->tileset.family == SectorTilesetFamily::New;
}

int sectorSurfaceVariantCount(pf::render::InstalledSectorSheet const* sheet,
    std::string const& kind)
{
    // Guarded on the sheet the same way the surface lookup is: with no
    // atlas behind the handle there is no declared count to report. The
    // version 1 family declares no families either, and its loader leaves
    // the map empty, so both answer 0 for the same reason - no author ever
    // stated a count.
    if (!sheetHasArtwork(sheet)) return 0;
    return sheet->tileset.variantCount(kind);
}

int SectorTileset::variantCount(std::string const& kind) const
{
    auto const found = variantCounts.find(kind);
    return found == variantCounts.end() ? 0 : found->second;
}

bool sectorTileSurfaceIsInView(pf::render::InstalledSectorSheet const* sheet,
    WorldDrawList const* list, Vec2 minimum,
    Vec2 maximum, float zoom)
{
    // Nothing is in view when the handle names no sheet: the loop this
    // early-out belongs to emits no cell without one, so a caller that
    // skips its work on this answer is skipping work the loop would never
    // have been given either (#1088).
    if (!sheetHasArtwork(sheet)) return false;
    float const cellWidth = CORE_CELL_WIDTH_PIXELS * zoom;
    float const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * zoom;
    if (cellWidth <= 0 || cellHeight <= 0) return false;
    auto const clipMin = list->GetClipRectMin();
    auto const clipMax = list->GetClipRectMax();
    return std::max(minimum.y, clipMin.y) < maximum.y
        && std::min(maximum.x, clipMax.x) > minimum.x;
}

bool drawSectorTileSurfacePerCell(pf::render::InstalledSectorSheet const* sheet,
    WorldDrawList* list, Vec2 minimum,
    Vec2 maximum, float zoom,
    SectorSurfaceTileResolver const& resolveTile)
{
    if (!sheetHasArtwork(sheet)) return false;
    // One tile per 1x1 world unit, magnified to the World cell's screen size
    // at the current zoom (the two cell sizes below). Partial levels are
    // cropped, never stretched to a Sector's height.
    //
    // The grid is anchored on the bottom of the drawn area, which for a
    // Sector is its floor line: the tile's own bottom edge lands there and
    // further rows stack upward. A Sector shorter than a full cell - a 0.7
    // Corridor, a 0.9 Room - keeps the end of the tile it was drawn beside,
    // the floor end, at one artwork pixel to one screen pixel, and loses the
    // top of the tile that would have stood above its ceiling. Anchoring on
    // the top instead cuts the floor off every short Sector and leaves the
    // tile's ceiling band sitting on the floor it is supposed to be above.
    //
    // Nothing wholly outside the clip is emitted, and the same question is
    // asked here as the one a caller in front of this loop asks before it
    // builds a per-cell answer it may never be asked for (#1071).
    if (!sectorTileSurfaceIsInView(sheet, list, minimum, maximum, zoom))
        return true;
    float const cellWidth = CORE_CELL_WIDTH_PIXELS * zoom;
    float const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * zoom;
    auto const clipMin = list->GetClipRectMin();
    auto const clipMax = list->GetClipRectMax();
    float const top = std::max(minimum.y, clipMin.y);
    float const right = std::min(maximum.x, clipMax.x);
    // The grid is counted from the area's own left edge and floor line, not
    // from the clip, and the resolver is asked in that frame. A Sector
    // scrolled half out of view therefore still resolves its own columns
    // and levels: the clip decides only which of them are worth emitting,
    // never which tile a cell wears or where its edges fall.
    int const columns = cellsCovered(maximum.x - minimum.x, cellWidth);
    int const rows = cellsCovered(maximum.y - minimum.y, cellHeight);
    int const firstColumn = std::max(0, static_cast<int>(
        (clipMin.x - minimum.x) / cellWidth));
    // Rows are counted up from the floor line, so the clip cannot move the
    // anchor; it only decides which rows are worth emitting. Everything
    // above `top` is left out, and so is every row that lies wholly below
    // the clip's bottom edge - but not a row the clip cuts through. That
    // last row is partly on screen and has to be emitted, at its full cell
    // rect and sampling its whole tile, for the scissor to crop it. Rounding
    // the distance up (`ceil`, which is what this line did until #1048)
    // rounds past exactly that row: with the clip's bottom edge one pixel
    // above a Sector's floor line, the whole bottom row of that Sector -
    // 319 of its 320 pixels visible - went undrawn. Truncation is the same
    // shape `firstColumn` two lines above already uses, and it stays exact at
    // the boundary: a clip bottom landing exactly on a row boundary leaves
    // that boundary's row below the cut, where it belongs, because the row
    // whose bottom edge *is* the cut lies under the clip rather than above
    // it.
    int const firstRow = std::max(0, static_cast<int>(
        (maximum.y - std::min(maximum.y, clipMax.y)) / cellHeight));
    for (int row = firstRow; row < rows; ++row) {
        float const rowBottom = maximum.y - float(row) * cellHeight;
        if (rowBottom <= top) break;
        float const rowTop = std::max(rowBottom - cellHeight, top);
        // Whatever of the tile stands above this row's top edge never reaches
        // the screen, so sampling starts that far down the tile.
        float const v0 = 1.0f - (rowBottom - rowTop) / cellHeight;
        for (int column = firstColumn; column < columns; ++column) {
            float const x = minimum.x + float(column) * cellWidth;
            if (x >= right) break;
            float const colRight = std::min(x + cellWidth, right);
            auto const tile = resolveTile(static_cast<std::uint32_t>(column),
                static_cast<std::uint32_t>(row));
            // A cell that resolves to no tile at all draws nothing. That is
            // not the missing-art case, which is "a tile was asked for and
            // this atlas has none of it" and answers in magenta; this is "no
            // tile was asked for". The Facade's doorframe pass is what
            // needs it (#1059): a Facade has one tile per door cell and no
            // surface tile anywhere else, because it is authored flat
            // colour (ADR 0003), so its resolver answers empty on every
            // cell the fill already covers. No Sector that draws a surface
            // today answers empty - its base is always a name - so nothing
            // that reaches the screen now changes.
            if (tile.empty()) continue;
            drawSurfaceCell(list, *sheet, tile,
                { x, rowTop }, { colRight, rowBottom }, 0.0f,
                (colRight - x) / cellWidth, v0, 1.0f);
        }
    }
    return true;
}

bool drawSectorTileSurface(pf::render::InstalledSectorSheet const* sheet,
    std::string const& kind, WorldDrawList* list,
    Vec2 minimum, Vec2 maximum, float zoom)
{
    // The per-cell loop with one name answered for every cell (#1039). A
    // Sector with no per-cell artwork resolves to exactly this, and it is
    // what a caller asks for when it has one tile and no wish to say which
    // cell wears it. The sheet the name is looked up in is the one the
    // caller handed over, which is the same sheet the pixels come from.
    return drawSectorTileSurfacePerCell(sheet, list, minimum, maximum, zoom,
        [&](std::uint32_t, std::uint32_t) { return kind; });
}

bool drawSectorTilePreview(pf::render::InstalledSectorSheet const* sheet,
    std::string const& kind, WorldDrawList* list,
    Vec2 minimum, Vec2 maximum)
{
    // No sheet behind the handle, or no tile under that name: nothing to
    // frame, and the caller is told so it can leave the swatch off rather
    // than show an empty one.
    if (!sheetHasArtwork(sheet) || !list) return false;
    auto const& tileset = sheet->tileset;
    auto const found = tileset.surfaces.find(kind);
    if (found == tileset.surfaces.end() || tileset.width <= 0
        || tileset.height <= 0)
        return false;
    // The rect may be handed over either way round; frame the tile inside
    // whichever corners it actually spans.
    auto const left = std::min(minimum.x, maximum.x);
    auto const right = std::max(minimum.x, maximum.x);
    auto const top = std::min(minimum.y, maximum.y);
    auto const bottom = std::max(minimum.y, maximum.y);
    // A rect with no area has nothing to frame, but the tile was found, so
    // this is a yes with no command rather than a missing tile.
    if (left == right || top == bottom) return true;
    // The whole tile, both fraction pairs running end to end. Nothing is
    // cropped and nothing is repeated: whatever the rect's aspect, the tile's
    // top-left lands on the rect's top-left and its bottom-right on the
    // rect's bottom-right. The authored render offset and drawn size are
    // World-space facts about how a Sector wears the tile and say nothing
    // about how a swatch frames it, so they are not consulted here.
    image(list, *sheet, found->second, { left, top }, { right, bottom }, 0.0f,
        1.0f, 0.0f, 1.0f);
    return true;
}

bool drawSectorTileBoundary(pf::render::InstalledSectorSheet const* sheet,
    char const* kind, WorldDrawList* list, Vec2 a, Vec2 b)
{
    if (!sheetHasArtwork(sheet)) return false;
    auto const& tileset = sheet->tileset;
    auto const found = tileset.boundaries.find(kind);
    if (found == tileset.boundaries.end() || tileset.width <= 0
        || tileset.height <= 0) return false;
    image(list, *sheet, found->second, a, b, 0.0f, 1.0f, 0.0f, 1.0f);
    return true;
}
