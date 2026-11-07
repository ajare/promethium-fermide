#include <utils/StringUtils.h>

#include "willpower/application/resourcesystem/ResourceExceptions.h"
#include "willpower/common/Exceptions.h"

#include "ProtoEntityResourceDefinitionFactory.h"

namespace applib {

using namespace std;
using namespace wp;

ProtoEntityResourceDefinitionFactory::ProtoEntityResourceDefinitionFactory(string const& factoryType)
    : ResourceDefinitionFactory("ProtoEntity", factoryType) {
}

shared_ptr<EntityHandler> ProtoEntityResourceDefinitionFactory::getEntityHandler(ProtoEntity* resource) {
  return resource->mEntityHandler;
}

shared_ptr<AnimationDatabase> ProtoEntityResourceDefinitionFactory::getAnimationDatabase(ProtoEntity* resource) {
  return resource->mAnimationDatabase;
}

void ProtoEntityResourceDefinitionFactory::loadExtraDefinitions(ProtoEntity* resource, wp::DataNode* node, entt::entity protoId) {
  resource->loadExtraDefinitions(node, protoId);
}

uint32_t ProtoEntityResourceDefinitionFactory::getAnimationIdFromName(string const& actor, string const& anim) {
  throw NotImplementedException();
}

void ProtoEntityResourceDefinitionFactory::create(application::resourcesystem::Resource* resource, application::resourcesystem::ResourceManager* resourceMgr, wp::DataNode* node) {
  auto entity = static_cast<ProtoEntity*>(resource);

  auto protoId = getEntityHandler(entity)->registerPrototype(entity->getName());
  entity->setComponentSystemId(protoId);

  createProtoEntity(entity, resourceMgr, node);
  loadExtraDefinitions(entity, node->getChild("Properties"), protoId);
}

}  // namespace applib