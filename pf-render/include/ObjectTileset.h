#pragma once
#include "SectorTileset.h"
#include "TileArtwork.h"
#include <map>
#include <optional>
#include <string>
#include <vector>
// One tile of an object-shaped atlas is the shared artwork type: a pixel
// region plus the render semantics the authored tileset declared for it
// (#1014).
using ObjectSprite = TileArtwork;
struct ObjectTileset
{
    std::filesystem::path image;
    int width{}, height{};
    std::map<std::string, ObjectSprite> sprites;
    // How many variants each base sprite declared (#1053). The version 2
    // object format derives a declared family the way the sector format
    // does, and the count is kept beside the sprites rather than counted back
    // off them, so it says what the author stated. A derived `-Vn` sprite is
    // not a base and holds no count of its own; a version 1 set declares no
    // families at all and so holds an empty map.
    std::map<std::string, int> variantCounts;
    // Resolves every sprite's cell and content rectangle into image pixels using
    // the manifest's own authored tile grid. That grid need not be the World
    // cell - it only has to divide the image evenly (#1006).
    static ObjectTileset load(std::filesystem::path const& path);
    // True when the tileset declares its own drawn size and render offset
    // per tile (version 2). A version 1 set derives them from each tile's
    // content rect instead, and so makes no declaration to check (#1022).
    bool declaresRenderSemantics{ false };
};

// What the World's object renderer requires of the atlas installed in its
// Objects slot (#1022).
//
// The renderer takes a tile's drawn size and render offset from the tileset
// rather than from a cell-sized quad, so an atlas whose declared values
// disagree with the domain's geometry would place a Door's artwork somewhere
// other than where its collision is. That is a worse failure than no
// artwork at all, so a World's Objects selection is checked rather than
// trusted.
//
// Each expectation is the geometry the renderer draws that tile into. Where
// the domain fixes it, it is composed from core's own constants: the Door
// leaf is the doorway minus its x insets, the Lift car the cell minus the
// car border, the Shuttle cells the car height. Where the renderer draws in
// screen space rather than World units - the Marker icon - or the extent is
// an artwork fact rather than a physical one - the Walkway and Platform lift
// slabs - the expectation is the authored reference extent the shipped set
// declares, which still catches a set that disagrees.
struct ObjectTileExpectation
{
    char const* tileName;
    float drawnWidth;
    float drawnHeight;
    pf::render::Vec2 renderOffset{ 0.0f, 0.0f };
};

// Every tile the object renderer draws from the Objects slot, in the order
// it names them. A tile listed here but absent from the atlas is a refusal.
std::vector<ObjectTileExpectation> const& requiredObjectTiles();

// Empty when `sprites` satisfies the contract for `atlasName`; otherwise a
// multi-line diagnostic naming every tile that is missing, and for each
// mismatch the declared value beside the expected one. The message is
// written to be acted on: it names the tile, the atlas, and what differs.
std::string checkObjectAtlasContract(std::string const& atlasName,
    std::map<std::string, ObjectSprite> const& sprites);

void setObjectTileset(ObjectTileset tileset, TextureId texture);
void clearObjectTileset();
// Agent artwork is independently selected, even when it shares the image.
void setAgentTileset(ObjectTileset tileset, TextureId texture);
void clearAgentTileset();
// The sprite helpers cross to the command stream on the shared drawing
// types (#997): pf::render::Vec2 for the destination rectangle and a
// packed colour for the tint.
std::optional<Vec2> agentSpriteSize(char const* name);
bool drawAgentSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b,
    PackedColourValue tint = White);
void setFurnitureTileset(ObjectTileset tileset, TextureId texture);
void clearFurnitureTileset();
std::optional<Vec2> furnitureSpriteSize(char const* name);
bool drawFurnitureSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b);
bool hasObjectTileset();
std::optional<Vec2> objectSpriteSize(char const* name);
// The installed render semantics for one object tile: its render offset and
// drawn size in World units, and whether it is tintable. Nullopt when no
// object atlas is installed or the tile is not in it (#1014).
std::optional<TileArtwork> objectTileArtwork(std::string const& name);
// Destination is the object's physical screen bounds, not its padded storage cell.
// Source fractions allow moving door leaves to be cropped without squashing.
bool drawObjectSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b,
    PackedColourValue tint = White, Vec2 sourceMin = {0, 0}, Vec2 sourceMax = {1, 1});

// No ImDrawList-taking form: see SectorTileset.h. The headless harness
// records a command vector and replays it (#1000).
