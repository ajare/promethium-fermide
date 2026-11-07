#include <willpower/application/resourcesystem/ResourceExceptions.h>
#include <willpower/common/DataNode.h>

#include "ApplicationResources.h"
#include "Map.h"
#include "MapWorldDefinitionFactory.h"

namespace pf::application {

MapWorldDefinitionFactory::MapWorldDefinitionFactory()
    : applib::MapResourceDefinitionFactory(
          "PromethiumFermide", nullptr, nullptr, nullptr, nullptr) {}

void MapWorldDefinitionFactory::create(
    wp::application::resourcesystem::Resource* resource,
    wp::application::resourcesystem::ResourceManager*, wp::DataNode* node) {
  using namespace wp::application::resourcesystem;
  auto* map = dynamic_cast<Map*>(resource);
  if (!map) throw ResourceException(resource, "Expected an Application Map");
  node->requireOnlyChildren({"factory", "Resource"});
  auto const dependency = node->getChild("Resource")->getValue();
  auto worldResource = std::dynamic_pointer_cast<WorldResource>(map->getDependentResource(dependency));
  if (!worldResource || !worldResource->runtime())
    throw ResourceException(resource, "Map requires a created World resource dependency");

  // ResourceManager creates dependencies before the Map. Reuse the existing
  // resource - and with it the one runtime that owns its World and the
  // catalogues that World resolved - instead of deserializing a second copy
  // (#970).
  map->setWorldResource(std::move(worldResource));
}

}  // namespace pf::application
