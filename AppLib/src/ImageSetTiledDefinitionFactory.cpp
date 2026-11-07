#include <utils/StringUtils.h>

#include "willpower/application/resourcesystem/ResourceExceptions.h"
#include "willpower/application/resourcesystem/XmlFileResource.h"
#include "willpower/common/Exceptions.h"

#include "ImageSetTiledDefinitionFactory.h"

namespace applib {
using namespace std;
using namespace utils;
using namespace wp;

ImageSetTiledDefinitionFactory::ImageSetTiledDefinitionFactory()
    : ImageSetResourceDefinitionFactory("Tiled") {
}

void ImageSetTiledDefinitionFactory::create(application::resourcesystem::Resource* resource, application::resourcesystem::ResourceManager* resourceMgr, wp::DataNode* node) {
  auto imageSetRes = static_cast<application::resourcesystem::ImageSetResource*>(resource);
  auto imageRes = static_cast<application::resourcesystem::ImageResource*>(imageSetRes->getDependentResource("Image").get());
  auto tiledRes = static_cast<application::resourcesystem::XmlFileResource*>(imageSetRes->getDependentResource("TileSpecs").get());

  auto tilesetNode = tiledRes->getReader()->getNode("tileset");

  // Get tile elements
  auto tileWidth = utils::StringUtils::parseInt(tilesetNode->getAttribute("tilewidth"));
  auto tileHeight = utils::StringUtils::parseInt(tilesetNode->getAttribute("tileheight"));

  // Check that the sizes are correct
  auto imageNode = tilesetNode->getChild("image");
  auto tileFileWidth = utils::StringUtils::parseInt(imageNode->getAttribute("width"));
  auto tileFileHeight = utils::StringUtils::parseInt(imageNode->getAttribute("height"));

  auto imageWidth = imageRes->getWidth();
  auto imageHeight = imageRes->getHeight();

  if (tileFileWidth != imageWidth || tileFileHeight != imageHeight) {
    string errMsg = "size of image specified as dependent resource does not match the size of the image specified in the tileset.";
    throw application::resourcesystem::ResourceException(resource, errMsg);
  }

  // Read tiles.
  auto tileNode = tilesetNode->getOptionalChild("tile");
  if (tileNode) {
    // Calculate x / y from id
    int tilesX = tileFileWidth / tileWidth;

    do {
      auto tileName = tileNode->getAttribute("id");
      int id = utils::StringUtils::parseInt(tileName);
      int x = (id % tilesX) * tileWidth;
      int y = (id / tilesX) * tileHeight;

      validateCoordinates(imageSetRes, tileName, x, y, tileWidth, tileHeight);

      application::resourcesystem::ImageSetResource::ImageDefinition imageDef;

      imageDef.x = x;
      imageDef.y = y;
      imageDef.width = tileWidth;
      imageDef.height = tileHeight;

      calculateUvCoords(imageSetRes, &imageDef);
      addImageDefinition(imageSetRes, tileName, imageDef);

    } while (tileNode->next());
  }
}

}  // namespace applib
