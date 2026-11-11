#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include "TileArtwork.h"
#include "WorldDrawList.h"

// The tilesets' installed-artwork handle, from the shared drawing vocabulary.
// The renderer never dereferences it: it only separates "an atlas is
// installed" from "nothing is", which is what makes a missing atlas draw
// magenta instead of drawing nothing (#1003).
using pf::render::TextureId;

// Which artwork family a sector tileset belongs to, taken from the file's
// own `version`. The two are the same fact stated twice: the version says
// which format the file is written in, the family says what that format
// promises about every Sector type's surface.
//
// The Old family is the version 1 draft: explicit pixel regions on an
// irregular sheet. It declares `shuttle` and `staircase` regions that
// nothing ever drew, because the Shuttle's rail corridor and the
// Staircase's steps are procedural and both wore the plain Lift and
// Stairwell shafts underneath them.
//
// The New family is the version 2 authored grid, where every
// surface-bearing Sector type wears its own full-cell tile (#1017).
//
// The family is a different question from whether a name is present. The
// old sheet has always carried both of those regions, so a renderer that
// repoints a Sector type on presence alone repaints every World that was
// drawn on the old atlas (#1027).
enum class SectorTilesetFamily
{
    Old,
    New
};

// A version 1 sector tileset names explicit pixel regions: its draft artwork
// is not a uniform grid. A version 2 set is authored on the uniform grid
// the object atlases use and carries no boundary artwork at all (#1017).
struct SectorTileset
{
    std::filesystem::path image;
    int width{}, height{};
    // The family the file's `version` puts it in. A set built by hand - a
    // test fixture, or an install that never reached its authored file -
    // starts in the Old family, which is the family every existing World
    // was drawn in: it borrows rather than reaching for a name it was
    // never told to draw (#1027).
    SectorTilesetFamily family{ SectorTilesetFamily::Old };
    std::map<std::string, SectorTileRegion> surfaces;
    std::map<std::string, SectorTileRegion> boundaries;
    // How many variants each base surface declared (#1038). The count is the
    // author's own number, read off the base sprite rather than counted back
    // from the tiles derived from it, so it says what was stated rather than
    // what happened to be present. A derived `-Vn` name is not a base and
    // holds no count of its own.
    std::map<std::string, int> variantCounts;
    // The variant count declared for one base surface, or 0 when that name
    // declares no family. This is the query a caller asks instead of
    // hardcoding how many variants a Sector type has.
    int variantCount(std::string const& kind) const;
    // Every tile in the set with its authored render semantics, keyed by the
    // name the application manifest declares for it - a boundary keeps its
    // `boundary-` prefix here, because that is the name the install pairs
    // against the `ImageSet` (#1014). A version 1 sector tileset declares
    // regions only, so these are the format's own semantics: a surface
    // repeats once per World unit and nothing in the set is tinted. A
    // version 2 set authors those semantics in the file, and leaves
    // `boundaries` empty because it draws no edge artwork (#1017).
    TileArtworkMap tiles;
    static SectorTileset load(std::filesystem::path const& path);
};
void setSectorTileset(SectorTileset tileset, TextureId texture);
void clearSectorTileset();
// Whether the installed atlas carries a surface tile under this name. A
// caller that has a type-specific tile and a borrowed one asks this before
// drawing, so an atlas that predates the specific tile keeps drawing the
// borrowed art instead of the missing-tile magenta (#1017).
//
// This answers "is the name there", not "does this family want the tile
// drawn". For a surface the old family declared and never drew - the
// Shuttle's and the Staircase's - it is the wrong question, and the
// caller asks the family below instead (#1027).
bool hasSectorTileSurface(std::string const& kind);
// Whether the installed sector atlas is the New family: a version 2
// authored set, in which every surface-bearing Sector type wears its own
// tile. False when no atlas is installed, which is the safe answer: with
// nothing installed there is no own art to wear.
bool sectorTilesetIsNewFamily();
// How many surface variants the installed atlas declares for one Sector
// type (#1038), which is what an editor sizes a swatch strip from rather
// than assuming a number. 0 when no atlas is installed, when the installed
// set is the version 1 family - which declares no families at all - or when
// the named type declares none.
int sectorSurfaceVariantCount(std::string const& kind);
// Which tile one cell of a Sector surface wears (#1039). `column` counts
// World cells from the drawn area's left edge and `level` from its floor
// line, so for a Sector's own area both are Sector-local: column 0 is the
// Sector's leftmost cell and level 0 the cell standing on its floor.
//
// The surface loop asks this for every cell it draws rather than being
// handed one name for the whole area. That is what the seam is for. The
// loop reproduces the floor-anchored crop and the culling the #1037
// contract pins, and a variant is meant to change the picture and nothing
// else - so the question of which tile is asked per cell, where a per-cell
// answer can be added, instead of in the loop, which would then have to
// change again the day a cell got to wear something else.
using SectorSurfaceTileResolver = std::function<std::string(
    std::uint32_t column, std::uint32_t level)>;

// The tileset helpers cross to the command stream on the shared drawing
// types (#997), so a caller hands them pf::render::Vec2 and a packed colour.
//
// Draws a Sector's surface as one tile per World cell: the area is cut
// into the cells it covers and every cell is resolved, drawn and cropped on
// its own. `minimum` and `maximum` are the drawn area's screen corners in
// the renderer's orientation, with `maximum.y` the floor line and
// `minimum.y` the ceiling.
//
// Rows stack upward from the floor line and columns rightward from the
// left edge, one cell each. Only the top row is ever shorter than a cell,
// and it is anchored on its own bottom edge and cropped at its top at one
// artwork pixel to one screen pixel - never stretched into the partial
// rect. Nothing outside the draw list's clip rectangle is emitted, and the
// clip never moves the anchor or changes which tile a cell wears.
//
// Returns false when no atlas is installed, which is the caller's cue to
// fall back to a plain fill. Returns true when the surface was drawn,
// including when a cell resolved to a name the atlas does not carry and
// drew the missing-tile magenta for that cell alone.
bool drawSectorTileSurfacePerCell(WorldDrawList* drawList, Vec2 minimum,
    Vec2 maximum, PackedColourValue tint, float zoom,
    SectorSurfaceTileResolver const& resolveTile);

// The same per-cell loop with one name answered for every cell, which is
// what a Sector with no per-cell artwork resolves to (#1039).
bool drawSectorTileSurface(std::string const& kind, WorldDrawList* drawList,
    Vec2 minimum, Vec2 maximum, PackedColourValue tint, float zoom = 1.0f);
bool drawSectorTileBoundary(char const* kind, WorldDrawList* drawList,
    Vec2 minimum, Vec2 maximum);

// No ImDrawList-taking form: a check that wants these on a live ImGui draw
// list records a command vector and replays it from the headless harness
// (#1000). Nothing ImGui is named here: the tileset crossing used to carry
// an ImGui texture id, and the shared renderer has no reason to know that
// type exists (#1003).
