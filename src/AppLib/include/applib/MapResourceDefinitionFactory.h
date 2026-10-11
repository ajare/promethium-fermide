#pragma once

#include <string>

#include "willpower/application/resourcesystem/Resource.h"
#include "willpower/application/resourcesystem/ResourceFactory.h"
#include "willpower/application/resourcesystem/ResourceDefinitionFactory.h"

#include "willpower/geometry/Mesh.h"

#include "Platform.h"
#include "Map.h"

namespace applib {

class APPLIB_API MapResourceDefinitionFactory : public wp::application::resourcesystem::ResourceDefinitionFactory {
  wp::geometry::UserAttributesFactory* mVertexAttributeFactory;

  wp::geometry::UserAttributesFactory* mEdgeAttributeFactory;

  wp::geometry::UserAttributesFactory* mPolygonAttributeFactory;

  wp::geometry::UserAttributesFactory* mPolygonVertexAttributeFactory;

protected:
  void createMesh(Map* map);

public:
  MapResourceDefinitionFactory(std::string const& factoryType,
                               wp::geometry::UserAttributesFactory* vertexAttributeFactory,
                               wp::geometry::UserAttributesFactory* edgeAttributeFactory,
                               wp::geometry::UserAttributesFactory* polygonAttributeFactory,
                               wp::geometry::UserAttributesFactory* polygonVertexAttributeFactory);

  ~MapResourceDefinitionFactory();
};

}  // namespace applib