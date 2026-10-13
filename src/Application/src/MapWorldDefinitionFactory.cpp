#include <core/World.h>
#include <core/YamlSerializer.h>
#include <willpower/application/resourcesystem/ResourceExceptions.h>
#include <willpower/application/resourcesystem/TextFileResource.h>
#include <willpower/common/DataNode.h>

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
  auto source = std::dynamic_pointer_cast<TextFileResource>(map->getDependentResource(dependency));
  if (!source) throw ResourceException(resource, "Map World source must be a TextFile");

  // Build a candidate before replacing the current World. A malformed document
  // must never discard a previously valid instance.
  auto serializer = core::YamlSerializer::fromString(source->getText());
  serializer->deserialize();
  auto world = std::make_unique<core::World>(map->getName(), 1, 1);
  core::SerializationWorkData work;
  if (!world->deserialize(*serializer, work)) {
    std::string message = "Could not load World from YAML";
    for (auto const& error : world->getDeserializationErrors()) message += "\n" + error;
    throw ResourceException(resource, message);
  }
  map->setWorld(std::move(world));
}

}  // namespace pf::application
