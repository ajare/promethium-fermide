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

// A version 1 sector tileset names explicit pixel regions: its draft artwork
// is not a uniform grid. A version 2 set is authored on the uniform grid
// the object atlases use and carries no boundary artwork at all (#1017).
struct SectorTileset
{
    std::filesystem::path image;
    int width{}, height{};
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
bool hasSectorTileSurface(std::string const& kind);
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
