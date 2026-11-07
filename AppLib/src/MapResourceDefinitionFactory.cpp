#include "willpower/application/resourcesystem/ResourceExceptions.h"

#include "MapResourceDefinitionFactory.h"

namespace applib {

using namespace std;
using namespace wp;

MapResourceDefinitionFactory::MapResourceDefinitionFactory(string const& factoryType,
                                                           wp::geometry::UserAttributesFactory* vertexAttributeFactory,
                                                           wp::geometry::UserAttributesFactory* edgeAttributeFactory,
                                                           wp::geometry::UserAttributesFactory* polygonAttributeFactory,
                                                           wp::geometry::UserAttributesFactory* polygonVertexAttributeFactory)
    : ResourceDefinitionFactory("Map", factoryType), mVertexAttributeFactory(vertexAttributeFactory), mEdgeAttributeFactory(edgeAttributeFactory), mPolygonAttributeFactory(polygonAttributeFactory), mPolygonVertexAttributeFactory(polygonVertexAttributeFactory) {
}

MapResourceDefinitionFactory::~MapResourceDefinitionFactory() {
  delete mVertexAttributeFactory;
  delete mEdgeAttributeFactory;
  delete mPolygonAttributeFactory;
  delete mPolygonVertexAttributeFactory;
}

void MapResourceDefinitionFactory::createMesh(Map* resource) {
  resource->mMesh = make_shared<geometry::Mesh>(
      mVertexAttributeFactory,
      mEdgeAttributeFactory,
      mPolygonAttributeFactory,
      mPolygonVertexAttributeFactory);
}

}  // namespace applib