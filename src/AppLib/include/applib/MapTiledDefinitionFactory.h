#pragma once

#include "Platform.h"
#include "MapResourceDefinitionFactory.h"
#include "willpower/common/DataNode.h"
// Tiled .tmx files really are XML, and are read through XmlFileResource. Only
// the resource *definition* moved to DataNode; the map data itself has not.
#include "willpower/common/XmlReader.h"

namespace applib {
class APPLIB_API MapTiledDefinitionFactory : public MapResourceDefinitionFactory {
  // NOTE: in order for hashing to work, this cannot be any higher.
  static const int MaxDimension = 128;

private:
  struct Strip {
    uint32_t x, y, w, h;
    std::string id;
  };

private:
  std::vector<std::string> parseCsv(wp::application::resourcesystem::Resource* resource, std::string const& data, int width, int height) const;

  std::vector<std::string> getLayerCellData(wp::application::resourcesystem::Resource* resource, wp::XmlNode const* layerNode, int idOffset, int* width, int* height) const;

  std::vector<Strip> generateStrips(std::vector<std::string> const& cells, int width, int height, size_t maxDim) const;

public:
  MapTiledDefinitionFactory();

  void create(wp::application::resourcesystem::Resource* resource, wp::application::resourcesystem::ResourceManager* resourceMgr, wp::DataNode* node) override;
};

}  // namespace applib
