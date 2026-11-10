#pragma once
#include <filesystem>
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
// The tileset helpers cross to the command stream on the shared drawing
// types (#997), so a caller hands them pf::render::Vec2 and a packed colour.
bool drawSectorTileSurface(std::string const& kind, WorldDrawList* drawList,
    Vec2 minimum, Vec2 maximum, PackedColourValue tint, float zoom = 1.0f);
bool drawSectorTileBoundary(char const* kind, WorldDrawList* drawList,
    Vec2 minimum, Vec2 maximum);

// No ImDrawList-taking form: a check that wants these on a live ImGui draw
// list records a command vector and replays it from the headless harness
// (#1000). Nothing ImGui is named here: the tileset crossing used to carry
// an ImGui texture id, and the shared renderer has no reason to know that
// type exists (#1003).
