#include <utils/StringUtils.h>

#include "willpower/application/resourcesystem/ResourceExceptions.h"
#include "willpower/common/Exceptions.h"

#include "Map.h"
#include "MapDefaultDefinitionFactory.h"
#include "MapGeometryObjectAttributes.h"

namespace applib {
using namespace std;
using namespace utils;
using namespace wp;

MapDefaultDefinitionFactory::MapDefaultDefinitionFactory()
    : MapResourceDefinitionFactory("",
                                   new VertexAttributeFactory,
                                   new EdgeAttributeFactory,
                                   new PolygonAttributeFactory,
                                   new PolygonVertexAttributeFactory) {
}

void MapDefaultDefinitionFactory::create(application::resourcesystem::Resource* resource, application::resourcesystem::ResourceManager* resourceMgr, wp::DataNode* node) {
  auto mapRes = static_cast<Map*>(resource);

  // Create mesh
  createMesh(mapRes);

  // Load from code, or wherver
  auto sourceNode = node->getChild("Source");
  auto sourceType = sourceNode->getProperty("type");

  if (sourceType == "code") {
    auto funcName = sourceNode->getValue();
    auto func = mapRes->getMeshCreationFunction(funcName);

    func(mapRes->getMesh().get(), resourceMgr);
  } else {
    throw application::resourcesystem::ResourceException(mapRes, "Unknown source type: " + sourceType);
  }
}

}  // namespace applib
