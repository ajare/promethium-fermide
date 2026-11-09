#include "TileArtwork.h"

#include <stdexcept>

TileArtworkMap installAtlasTiles(TileArtworkMap const& authored,
    AtlasPixelRegions const& regions, int imageWidth, int imageHeight,
    std::string const& atlasName)
{
    TileArtworkMap installed;
    for (auto const& [name, region] : regions) {
        // The authored tileset is the only source of render semantics. A
        // tile the atlas declares that the tileset never authored has no
        // declared visual extent, so installing it would put artwork where
        // nothing says it belongs.
        auto const found = authored.find(name);
        if (found == authored.end())
            throw std::runtime_error("Atlas '" + atlasName + "' declares tile '" + name
                + "' that its authored tileset never defines");
        auto const& artwork = found->second;
        if (region.x < 0 || region.y < 0 || region.width <= 0 || region.height <= 0
            || region.x + region.width > imageWidth
            || region.y + region.height > imageHeight)
            throw std::runtime_error("Atlas '" + atlasName + "' tile '" + name
                + "' pixel region is outside the atlas image");
        installed.emplace(name, TileArtwork{
            SectorTileRegion{ region.x, region.y, region.width, region.height },
            artwork.tintable, artwork.offset, artwork.drawnSize });
    }
    return installed;
}
