#include "ObjectTileset.h"
#include "AuthoredTileGrid.h"
#include "core/Defines.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
namespace {
ObjectTileset active;
TextureId textureId{};
ObjectTileset agents;
TextureId agentTextureId{};
ObjectTileset furniture;
TextureId furnitureTextureId{};
}
ObjectTileset ObjectTileset::load(std::filesystem::path const& path)
{
    auto root = YAML::LoadFile(path.string());
    int const version = root["version"].as<int>();
    // Version 1 declares a region and a tintable flag per tile. Version 2
    // adds the render offset and the drawn size, so a tile's visual extent
    // is authored rather than inferred from whatever object draws it
    // (#1014).
    if (version != 1 && version != 2)
        throw std::runtime_error("Unsupported object tileset version");
    ObjectTileset result;
    result.declaresRenderSemantics = version >= 2;
    result.image = path.parent_path() / root["image"].as<std::string>();
    // The grid resolution is shared with the sector atlas, which authors the
    // same cell-and-content shape (#1017).
    //
    // A version 2 object sprite may declare a variant family, and the shared
    // grid derives it: `variants: N` yields `-V1` .. `-VN` in the base's own
    // column one row below the last, each inheriting the base's content
    // rectangle, render offset, drawn size and tintable flag. That is the
    // sector format's promise, kept here by the same code rather than by a
    // second copy that could drift from it (#1053).
    //
    // The version 1 object format does not take the key. It authors no render
    // semantics at all - a v1 tile's drawn size is whatever its content
    // rectangle measures over the grid - so a family stacked beneath such a
    // base is a family nobody sized, and the version 1 set is the legacy
    // custom-theme shape that nothing new is authored in. Refused rather than
    // quietly derived, because an author who wrote the key meant to gain the
    // capability and would not have gained it.
    auto const variantFamilies = version >= 2
        ? AuthoredVariantFamilies::Derived
        : AuthoredVariantFamilies::Refused;
    auto const grid = loadAuthoredTileGrid(root, version >= 2, "object sprite",
        variantFamilies);
    result.width = grid.width;
    result.height = grid.height;
    result.sprites = grid.tiles;
    // The count travels with the sprites for the same reason the sector's
    // does: a map of resolved regions cannot say that `door` declared three
    // variants, and anything that later sizes a strip of Door leaves from the
    // set would rather size it from what was stated than from a constant
    // (#1053).
    result.variantCounts = grid.variantCounts;
    // A version 2 set authors nothing for the Force Bridge, and is refused
    // if it tries. A bridge's span is continuous and arbitrary, so a fixed
    // tile would be stretched wrong at most lengths it is drawn at; the
    // renderer's primitive line stays. Refusing it here is what keeps that
    // decision from being quietly reversed by a tile added to a sheet
    // nobody thought about (#1019).
    if (version >= 2 && result.sprites.count("force-bridge"))
        throw std::runtime_error("The Force Bridge is rendered as a primitive "
            "line and is deliberately given no tile: its span is arbitrary, so "
            "a fixed tile would be wrong at most lengths");
    // The required-tile list is the version 1 object atlas's own contract.
    // A version 2 set is a different authored set with a different shape;
    // which tiles a World requires from it is validated where the World's
    // atlas selection is resolved, not by holding every old set's list.
    if (version == 1)
    {
        for (auto name : {"door", "button-enabled", "button-disabled", "window-clear", "window-frosted",
            "window-tinted", "booth-window-open", "booth-window-closed", "agent", "marker", "platform-lift", "shuttle-car-left",
            "shuttle-car-middle", "shuttle-car-right", "shuttle-car-connector", "lift-car"})
            if (!result.sprites.count(name)) throw std::runtime_error("Missing object sprite: " + std::string(name));
        if (!result.sprites.at("agent").tintable)
            throw std::runtime_error("Agent sprite must support tinting");
    }
    return result;
}
void setObjectTileset(ObjectTileset tileset, TextureId texture)
{
    active = std::move(tileset); textureId = texture;
}
void clearObjectTileset() { textureId = {}; active = {}; }
void setAgentTileset(ObjectTileset tileset, TextureId texture)
{
    agents = std::move(tileset); agentTextureId = texture;
}
void clearAgentTileset() { agents = {}; agentTextureId = {}; }
std::optional<Vec2> agentSpriteSize(char const* name)
{
    auto const found = agents.sprites.find(name);
    if (!agentTextureId || agents.width <= 0 || agents.height <= 0 || found == agents.sprites.end())
        return std::nullopt;
    return Vec2{float(found->second.region.width), float(found->second.region.height)};
}
bool drawAgentSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b, PackedColourValue tint)
{
    if (!agentSpriteSize(name)) return false;
    auto const& sprite = agents.sprites.at(name);
    auto const& r = sprite.region;
    Vec2 uv0{(r.x + .5f) / agents.width, (r.y + .5f) / agents.height};
    Vec2 uv1{(r.x + r.width - .5f) / agents.width, (r.y + r.height - .5f) / agents.height};
    // Whether the resolved Agent colour reaches the sheet at all is the tile's
    // authored fact, exactly as it is for an Object sprite. The Agent's colour,
    // its Colour tags and the selection gold are still resolved by the caller;
    // `tintable` says whether this artwork takes a tint (#1018).
    list->AddImage(WorldDrawList::Texture::AgentAtlas, a, b, uv0, uv1,
        sprite.tintable ? tint : White);
    return true;
}
void setFurnitureTileset(ObjectTileset tileset, TextureId texture)
{
    furniture = std::move(tileset); furnitureTextureId = texture;
}
void clearFurnitureTileset() { furniture = {}; furnitureTextureId = {}; }
std::optional<Vec2> furnitureSpriteSize(char const* name)
{
    auto const found = furniture.sprites.find(name);
    if (!furnitureTextureId || furniture.width <= 0 || furniture.height <= 0 || found == furniture.sprites.end())
        return std::nullopt;
    return Vec2{float(found->second.region.width), float(found->second.region.height)};
}
bool drawFurnitureSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b)
{
    if (!furnitureSpriteSize(name)) return false;
    auto const& r = furniture.sprites.at(name).region;
    Vec2 minimum{std::min(a.x, b.x), std::min(a.y, b.y)};
    Vec2 maximum{std::max(a.x, b.x), std::max(a.y, b.y)};
    list->AddImage(WorldDrawList::Texture::FurnitureAtlas, minimum, maximum,
        {(r.x + .5f) / furniture.width, (r.y + .5f) / furniture.height},
        {(r.x + r.width - .5f) / furniture.width, (r.y + r.height - .5f) / furniture.height});
    return true;
}
bool hasObjectTileset() { return textureId != TextureId{}; }
std::optional<Vec2> objectSpriteSize(char const* name)
{
    auto const sprite = active.sprites.find(name);
    if (!textureId || sprite == active.sprites.end()) return std::nullopt;
    return Vec2{static_cast<float>(sprite->second.region.width),
        static_cast<float>(sprite->second.region.height)};
}
std::optional<TileArtwork> objectTileArtwork(std::string const& name)
{
    auto const sprite = active.sprites.find(name);
    if (!textureId || sprite == active.sprites.end()) return std::nullopt;
    return sprite->second;
}
bool drawObjectSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b,
    PackedColourValue tint, Vec2 sourceMin, Vec2 sourceMax)
{
    auto const found = active.sprites.find(name);
    if (!textureId || found == active.sprites.end()) return false;
    Vec2 minimum{std::min(a.x, b.x), std::min(a.y, b.y)};
    Vec2 maximum{std::max(a.x, b.x), std::max(a.y, b.y)};
    if (minimum.x == maximum.x || minimum.y == maximum.y) return true;
    auto const& sprite = found->second;
    auto const& r = sprite.region;
    auto uv = [&](Vec2 fraction) {
        return Vec2{(r.x + 0.5f + fraction.x * (r.width - 1)) / active.width,
            (r.y + 0.5f + fraction.y * (r.height - 1)) / active.height};
    };
    list->AddImage(WorldDrawList::Texture::ObjectAtlas, minimum, maximum,
        uv(sourceMin), uv(sourceMax), sprite.tintable ? tint : White);
    return true;
}

// --- The Objects atlas contract (#1022) -------------------------------
//
// The renderer takes a tile's drawn size and render offset from the
// tileset, so the tileset is now the authority on where a Door's artwork
// lands relative to the Door's collision. A World may select which atlas
// fills that role, which makes the selection a load-time question rather
// than a build-time one: the answer cannot be reviewed alongside the art.
// This is the check that answers it.
//
// It runs only against a tileset that declares its own render semantics
// (version 2). A version 1 set derives them from each tile's content
// rect, makes no declaration of its own, and is the artwork every existing
// World already draws - checking it here would refuse the default.

namespace
{
    // The declared values arrive from YAML text and the expected ones are
    // composed from core's constants, so exact equality would refuse the
    // shipped set over a rounding difference in its last digit. A real
    // misalignment is orders of magnitude larger than this.
    constexpr float kDrawnTolerance = 1e-4f;

    bool sameExtent(float declared, float expected)
    {
        return std::abs(declared - expected) <= kDrawnTolerance;
    }

    bool sameOffset(pf::render::Vec2 const& declared, pf::render::Vec2 const& expected)
    {
        return sameExtent(declared.x, expected.x) && sameExtent(declared.y, expected.y);
    }

    std::string extent(float w, float h)
    {
        std::ostringstream text;
        text << std::setprecision(6) << w << " x " << h;
        return text.str();
    }

    std::string offset(pf::render::Vec2 const& v)
    {
        std::ostringstream text;
        text << std::setprecision(6) << "(" << v.x << ", " << v.y << ")";
        return text.str();
    }
}

std::vector<ObjectTileExpectation> const& requiredObjectTiles()
{
    // The list mirrors the names the drawing code asks the Objects slot
    // for, and nothing else. A tile added to a draw call without being
    // added here goes unchecked; a tile kept here that no draw call names
    // refuses an atlas over artwork nothing draws, which is the shape
    // `booth-window-closed` had before #1031 dropped it. The render smoke
    // module reads every source that names a tile to a draw call - the
    // renderer and the editor - and requires every entry here to be named
    // by one, and every name those calls ask for to be a tile the installed
    // atlas carries, so the review the two lists used to need is a check
    // rather than a habit (#1064).
    static std::vector<ObjectTileExpectation> const tiles{
        // A Door leaf is a full World cell. The renderer draws it at the
        // cell rect and clips it to the doorway aperture, so the tile's
        // own extent is the cell rather than the aperture it slides
        // through: the clip, not the drawn size, is what keeps the leaf
        // off the wall. Every leaf tile carries the same drawn size and a
        // zero offset, which is what lets one contract value cover the
        // whole family instead of one per width and height (#1056).
        ObjectTileExpectation{"door-L0", 1.0f, 1.0f, {0.0f, 0.0f}},
        ObjectTileExpectation{"door-R0", 1.0f, 1.0f, {0.0f, 0.0f}},
        // A Lift landing Door and a Shuttle hatch draw their own leaves,
        // chosen by the transport that owns the Door rather than by the
        // wall it sits in. Same full-cell contract as the ordinary leaf,
        // because the same aperture clip bounds them (#1057). A Shuttle
        // hatch is one cell wide, so its family carries no right-hand
        // leaf and no expectation here.
        ObjectTileExpectation{"door-lift-L0", 1.0f, 1.0f, {0.0f, 0.0f}},
        ObjectTileExpectation{"door-lift-R0", 1.0f, 1.0f, {0.0f, 0.0f}},
        ObjectTileExpectation{"door-shuttle-L0", 1.0f, 1.0f, {0.0f, 0.0f}},
        // A tall ordinary Door wears its own pair, chosen by its height
        // class. Same full-cell contract as every other leaf: the taller
        // aperture is what the clip cuts the tile to, so the declared
        // drawn size stays one cell and one value covers the family
        // (#1058).
        ObjectTileExpectation{"door-tall-L0", 1.0f, 1.0f, {0.0f, 0.0f}},
        ObjectTileExpectation{"door-tall-R0", 1.0f, 1.0f, {0.0f, 0.0f}},
        // A Bulkhead Door is a slab, not a doorway: its own width, spanning
        // the corridor's full height.
        ObjectTileExpectation{"bulkhead-door",
            CORE_BULKHEAD_DOOR_WIDTH, CORE_CORRIDOR_HEIGHT, {0.0f, 0.0f}},
        // The Access panel tile carries the panel's default geometry. An
        // authored scale multiplies the object and its artwork together, so
        // the declared extent stays the default.
        ObjectTileExpectation{"access-panel", 0.5f, 0.25f, {0.0f, 0.0f}},
        ObjectTileExpectation{"window-clear",
            1.0f - 2.0f * CORE_WINDOW_X_INSET, CORE_WINDOW_HEIGHT, {0.0f, 0.0f}},
        ObjectTileExpectation{"window-frosted",
            1.0f - 2.0f * CORE_WINDOW_X_INSET, CORE_WINDOW_HEIGHT, {0.0f, 0.0f}},
        ObjectTileExpectation{"window-tinted",
            1.0f - 2.0f * CORE_WINDOW_X_INSET, CORE_WINDOW_HEIGHT, {0.0f, 0.0f}},
        ObjectTileExpectation{"booth-window-open",
            1.0f - 2.0f * CORE_WINDOW_X_INSET, CORE_WINDOW_HEIGHT, {0.0f, 0.0f}},
        // The shutter is drawn inside the frame's aperture, inset by the
        // frame's own wall thickness: 5 px of the 52 px frame and 5 px of
        // the 48 px height. The renderer insets by the same fractions, so
        // the shutter's declared extent has to be the aperture it slides in.
        ObjectTileExpectation{"booth-window-shutter",
            (1.0f - 2.0f * CORE_WINDOW_X_INSET) * 42.0f / 52.0f,
            CORE_WINDOW_HEIGHT * 38.0f / 48.0f, {0.0f, 0.0f}},
        // The button is square in World units only after the cell's y:x
        // render ratio is undone: 0.1 wide, and that divided by the ratio
        // tall. The renderer draws it at that extent, so a set that
        // declares a taller button paints outside the object.
        ObjectTileExpectation{"button-enabled", CORE_BUTTON_SIZE,
            CORE_BUTTON_SIZE / CORE_CELL_YX_RENDER_RATIO, {0.0f, 0.0f}},
        ObjectTileExpectation{"button-disabled", CORE_BUTTON_SIZE,
            CORE_BUTTON_SIZE / CORE_CELL_YX_RENDER_RATIO, {0.0f, 0.0f}},
        // The Marker icon is a screen-space fact: a fixed pixel longest
        // edge that does not follow the World cell. Its declared drawn size
        // is the authored art's own reference extent rather than a World
        // geometry, and the renderer scales it in screen space - so this
        // guards the authored aspect rather than a collision alignment.
        ObjectTileExpectation{"marker", 0.25f, 0.1375f, {0.0f, 0.0f}},
        // The Walkway's physical shape is a zero-height line and must stay
        // that way; its visible slab hangs below the floor line of the cell
        // it was authored in. The offset is the whole of its placement, so
        // a wrong one moves the slab off the floor it supports.
        ObjectTileExpectation{"walkway", 1.0f, 0.075f, {0.0f, -0.075f}},
        // The Lift car is the cell less the car's border, at the car's
        // height - which is the doorway height plus its own sill.
        ObjectTileExpectation{"lift-car",
            1.0f - 2.0f * CORE_LIFT_CAR_BORDER, CORE_LIFT_CAR_HEIGHT, {0.0f, 0.0f}},
        // The Platform lift slab is as wide as the cell and 0.05 tall,
        // drawn from its own anchor.
        ObjectTileExpectation{"platform-lift", 1.0f, 0.05f, {0.0f, 0.0f}},
        // Each Shuttle carriage cell is one cell wide at the car height;
        // the renderer draws the three cells and the connector at the same
        // extent, so a mismatch shifts the carriage apart from its own
        // boarding positions.
        ObjectTileExpectation{"shuttle-L0", 1.0f, CORE_SHUTTLE_CAR_HEIGHT, {0.0f, 0.0f}},
        ObjectTileExpectation{"shuttle-M0", 1.0f, CORE_SHUTTLE_CAR_HEIGHT, {0.0f, 0.0f}},
        ObjectTileExpectation{"shuttle-R0", 1.0f, CORE_SHUTTLE_CAR_HEIGHT, {0.0f, 0.0f}},
        ObjectTileExpectation{"shuttle-connector", 1.0f, CORE_SHUTTLE_CAR_HEIGHT, {0.0f, 0.0f}},
    };
    return tiles;
}

std::string checkObjectAtlasContract(std::string const& atlasName,
    std::map<std::string, ObjectSprite> const& sprites)
{
    std::vector<std::string> problems;
    for (auto const& expected : requiredObjectTiles())
    {
        std::string const name{expected.tileName};
        auto const found = sprites.find(name);
        if (found == sprites.end())
        {
            problems.push_back("tile '" + name + "' is not in the atlas");
            continue;
        }
        auto const& art = found->second;
        if (!sameExtent(art.drawnSize.x, expected.drawnWidth)
            || !sameExtent(art.drawnSize.y, expected.drawnHeight))
        {
            problems.push_back("tile '" + name + "' declares a drawn size of "
                + extent(art.drawnSize.x, art.drawnSize.y)
                + "; the renderer draws it at " + extent(expected.drawnWidth, expected.drawnHeight));
        }
        if (!sameOffset(art.offset, expected.renderOffset))
        {
            problems.push_back("tile '" + name + "' declares a render offset of "
                + offset(art.offset) + "; the renderer draws it at "
                + offset(expected.renderOffset));
        }
    }
    if (problems.empty()) return {};
    std::ostringstream text;
    text << "Objects atlas '" << atlasName << "' cannot be used by this World: "
        << problems.size() << " tile(s) do not match what the object renderer draws";
    for (auto const& problem : problems) text << "\n  - " << problem;
    return text.str();
}
