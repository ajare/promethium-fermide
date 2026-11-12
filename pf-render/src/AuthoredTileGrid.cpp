#include "AuthoredTileGrid.h"

#include <cmath>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
// Reads one required pair of World-unit numbers out of a tileset node. A
// field the file never wrote, or one that is not a finite number, is a
// refusal rather than a default: an offset that quietly became zero would
// put artwork where no author said it goes.
pf::render::Vec2 requirePair(YAML::Node const& node, std::string const& tile,
    std::string const& tileNoun, char const* field)
{
    auto const value = node[field];
    if (!value.IsSequence() || value.size() != 2)
        throw std::runtime_error(tileNoun + " '" + tile + "' is missing its "
            + std::string(field) + " pair");
    double first{}, second{};
    try { first = value[0].as<double>(); second = value[1].as<double>(); }
    catch (YAML::Exception const&)
    {
        throw std::runtime_error(tileNoun + " '" + tile + "' " + field
            + " must be two numbers");
    }
    if (!std::isfinite(first) || !std::isfinite(second))
        throw std::runtime_error(tileNoun + " '" + tile + "' " + field
            + " must be finite");
    return { static_cast<float>(first), static_cast<float>(second) };
}

// Reads the `variants` key one base sprite declares. That count is the whole
// of the author's statement about a family - the derived tiles' names and
// cells come from it - so all the grid has to establish is that the key was
// allowed here, that it says a whole number, and that the column it
// describes has room to stand on the declared sheet. Once the count is
// settled the grid derives the family itself, so every format that takes the
// capability gets the same rule rather than one copy per loader (#1038,
// #1053).
std::optional<int> readVariantCount(YAML::Node const& sprite,
    std::string const& name, std::string const& tileNoun,
    AuthoredVariantFamilies variantFamilies, int row, int rows)
{
    auto const declared = sprite["variants"];
    if (!declared.IsDefined()) return std::nullopt;
    if (variantFamilies == AuthoredVariantFamilies::Refused)
        throw std::runtime_error(tileNoun + " '" + name + "' declares `variants`, "
            "which this format does not derive; the key belongs on a base "
            "sprite of a format that lays out the family column beneath it");
    int count{};
    try { count = declared.as<int>(); }
    catch (YAML::Exception const&)
    {
        throw std::runtime_error(tileNoun + " '" + name + "' variants must be a "
            "whole number");
    }
    if (count < 0)
        throw std::runtime_error(tileNoun + " '" + name + "' variants must not be "
            "negative");
    // The family descends from the row the base was authored on, so its last
    // variant sits at `row + count`. A count reaching past the last row of
    // the declared sheet describes a tile no cell exists for, and a sheet
    // that quietly stops short of what was asked is the silent failure a
    // refusal is here to avoid.
    if (row + count >= rows)
        throw std::runtime_error(tileNoun + " '" + name + "' declares "
            + std::to_string(count) + " variants, which would run past the bottom "
            "of the declared sheet: the base sits at row " + std::to_string(row)
            + " of " + std::to_string(rows));
    return count;
}

// One base sprite that declared a family, kept with the row it was authored
// on so the derivation does not have to recover that row from a region that
// has already had its content offset folded into it.
struct DeclaredFamily
{
    std::string name;
    int row{};
    int count{};
};

// `-V` followed by digits and nothing else: exactly the shape the variant
// derivation produces. A sprite authored under one of those names would
// collide with whatever the family rule derives, and which of the two won
// would come down to map order, so the name is refused before anything is
// derived (#1038). It is refused in every map of a file that has a
// derivation anywhere, not only in the map that derives - see
// `reserveVariantNames` in the header (#1069).
bool isDerivedVariantName(std::string const& name)
{
    auto const mark = name.rfind("-V");
    if (mark == std::string::npos || mark + 2 >= name.size()) return false;
    for (auto it = name.begin() + mark + 2; it != name.end(); ++it)
        if (!std::isdigit(static_cast<unsigned char>(*it))) return false;
    return true;
}

// Expands one declared family into the grid. Variant *n* is `<base>-Vn`, in
// the base's own column and one authored tile height further down per step,
// and it inherits the base's content rectangle, render offset, drawn size
// and tintable flag: adding a variant is a picture decision and never a
// re-declaration of geometry. The count was already established to fit the
// sheet, so every cell the family lands on exists.
void deriveVariantFamily(DeclaredFamily const& family,
    std::string const& tileNoun, AuthoredTileGrid& grid)
{
    auto const base = grid.tiles.find(family.name);
    // The family was read off the same sprite this tile came from, so the
    // base is here by construction. Saying so keeps a future edit from
    // deriving a family onto a tile that was never put in the map.
    if (base == grid.tiles.end())
        throw std::runtime_error(tileNoun + " '" + family.name
            + "' declared a variant family but is not in the grid it was "
            "declared on");
    for (int variant = 1; variant <= family.count; ++variant) {
        auto derived = base->second;
        derived.region.y += variant * grid.tileHeight;
        auto const variantName = family.name + "-V" + std::to_string(variant);
        // The pass over the authored names already refuses anything shaped
        // like a derived tile, so this is the backstop rather than the
        // guard: it keeps a derivation that somehow collided from being
        // silently swallowed, which is how a tile ends up drawing as
        // something else.
        if (!grid.tiles.emplace(variantName, derived).second)
            throw std::runtime_error(tileNoun + " '" + variantName + "' is "
                "derived from '" + family.name + "' and cannot also be authored "
                "beside the family it belongs to");
    }
}
} // namespace

AuthoredTileGrid loadAuthoredTileGrid(YAML::Node const& root,
    bool declaresRenderSemantics, std::string const& tileNoun,
    AuthoredVariantFamilies variantFamilies, std::string const& spritesKey,
    bool reserveVariantNames)
{
    AuthoredTileGrid grid;
    grid.width = root["size"][0].as<int>();
    grid.height = root["size"][1].as<int>();
    grid.tileWidth = root["tile_size"][0].as<int>();
    grid.tileHeight = root["tile_size"][1].as<int>();
    int const tw = grid.tileWidth, th = grid.tileHeight;
    // The authored tile grid is how the artist laid the image out; the World
    // cell is how large a World unit is on screen. They are not the same fact,
    // and the renderer stretches a resolved region into whatever destination it
    // is drawn into, so the grid is never compared with the cell. All that is
    // required is that whole authored cells cover the image with no remainder,
    // because a cell coordinate that leaves a remainder resolves to nothing
    // (#1006).
    if (tw <= 0 || th <= 0 || grid.width <= 0 || grid.height <= 0 ||
        grid.width % tw || grid.height % th)
        throw std::runtime_error("An authored tile grid must cover the atlas "
            "image with no remainder");
    auto const sprites = root[spritesKey];
    // Two different files both fail this test, and saying "Missing" about
    // the one where the author wrote the key is the wrong diagnosis. A key
    // the file never wrote hands back a zombie node that throws on any type
    // query, so presence is settled before the type is asked; both it and a
    // key written with no body - a defined Null - are the file declaring no
    // tiles of this kind, which is what `Missing` says. A scalar or a
    // sequence was written as a thing the grid cannot read, and is refused
    // as that (#1066).
    if (!sprites || sprites.IsNull())
        throw std::runtime_error("Missing " + tileNoun + " definitions");
    if (!sprites.IsMap())
        throw std::runtime_error("`" + spritesKey + "` is not a map of "
            + tileNoun + " definitions");
    int const rows = grid.height / th;
    // Families are derived in a second pass, after every authored sprite has
    // been read. Deriving inside this loop would let a base declared early
    // claim a `-Vn` name before a later authored sprite of that same name
    // had even been seen, and the refusal that came back would be a
    // duplicate-key complaint about the derivation rather than the plain
    // statement that the name is not the author's to write.
    std::vector<DeclaredFamily> families;
    for (auto const& item : sprites) {
        auto const v = item.second;
        if (!v["cell"].IsSequence() || v["cell"].size() != 2 ||
            !v["content"].IsSequence() || v["content"].size() != 4)
            throw std::runtime_error("Invalid " + tileNoun + " rectangle");
        auto const c = v["content"];
        int const column = v["cell"][0].as<int>(), row = v["cell"][1].as<int>();
        SectorTileRegion r{c[0].as<int>(), c[1].as<int>(), c[2].as<int>(), c[3].as<int>()};
        if (column < 0 || row < 0 || column >= grid.width / tw || row >= grid.height / th ||
            r.x < 0 || r.x >= tw || r.y < 0 || r.width <= 0 || r.height <= 0 || r.height > th ||
            r.width > grid.width || column * tw + r.x > grid.width - r.width || r.y > th - r.height)
            throw std::runtime_error(tileNoun + " is outside its cell");
        r.x += column * tw; r.y += row * th;
        auto const name = item.first.as<std::string>();
        // The reservation is asked on its own, not off `variantFamilies`.
        // This map may derive nothing at all and still sit in a file that
        // derives, and the shape belongs to the derivation either way: an
        // authored `-Vn` here reads as a family nothing made, and would be
        // the name a later family's derivation reaches (#1069). Where the
        // caller says no derivation exists anywhere in the format - the
        // version 1 object file - the shape is nobody's and is not refused.
        if (reserveVariantNames && isDerivedVariantName(name))
            throw std::runtime_error(tileNoun + " '" + name + "' uses a name the "
                "variant derivation reserves; a family's variants are derived "
                "from its base and are not authored beside it");
        TileArtwork artwork{ r, v["tintable"].as<bool>(), { 0.0f, 0.0f }, { 0.0f, 0.0f } };
        if (declaresRenderSemantics)
        {
            artwork.offset = requirePair(v, name, tileNoun, "offset");
            artwork.drawnSize = requirePair(v, name, tileNoun, "drawn_size");
            if (artwork.drawnSize.x <= 0.0f || artwork.drawnSize.y <= 0.0f)
                throw std::runtime_error(tileNoun + " '" + name
                    + "' drawn size must be positive");
            // One World unit of authored art is one authored tile, so a
            // drawn size larger than the image could never be drawn from it.
            if (artwork.drawnSize.x * tw > grid.width + 1e-4
                || artwork.drawnSize.y * th > grid.height + 1e-4)
                throw std::runtime_error(tileNoun + " '" + name
                    + "' drawn size does not fit the atlas image");
        }
        else
        {
            // A version 1 tileset authors no render semantics. The identity
            // is the format's own: no offset, and a drawn size of whatever
            // the content rectangle measures in World units over the
            // authored grid.
            artwork.drawnSize = { float(r.width) / float(tw), float(r.height) / float(th) };
        }
        if (!grid.tiles.emplace(name, artwork).second)
            throw std::runtime_error("Duplicate " + tileNoun);
        if (auto const count = readVariantCount(v, name, tileNoun,
                variantFamilies, row, rows))
        {
            grid.variantCounts.emplace(name, *count);
            families.push_back({ name, row, *count });
        }
    }
    for (auto const& family : families)
        deriveVariantFamily(family, tileNoun, grid);
    return grid;
}
