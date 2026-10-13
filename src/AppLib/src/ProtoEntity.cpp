#include "willpower/application/resourcesystem/ResourceExceptions.h"

#include "ProtoEntity.h"

namespace applib {

using namespace std;
using namespace wp;

ProtoEntity::ProtoEntity(string const& name,
                         string const& namesp,
                         string const& source,
                         map<string, string> const& tags,
                         application::resourcesystem::ResourceLocation* location,
                         shared_ptr<EntityHandler> entityHandler,
                         shared_ptr<AnimationDatabase> animDatabase)
    : application::resourcesystem::Resource(name, namesp, "ProtoEntity", source, tags, location), mEntityHandler(entityHandler), mAnimationDatabase(animDatabase) {
}

void ProtoEntity::loadExtraDefinitions(wp::DataNode* node, entt::entity protoId) {
  // To be implemented by child class.
}

void ProtoEntity::setComponentSystemId(entt::entity id) {
  mCompSysId = id;
}

entt::entity ProtoEntity::getComponentSystemId() const {
  return mCompSysId;
}

}  // namespace applib