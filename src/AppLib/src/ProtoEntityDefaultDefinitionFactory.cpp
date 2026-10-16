#include <format>
#include <utils/StringUtils.h>
#include "willpower/common/DataNode.h"

#include "willpower/application/resourcesystem/AnimationSetResource.h"
#include "willpower/application/resourcesystem/ResourceExceptions.h"
#include "willpower/common/Exceptions.h"

#include "ProtoEntity.h"
#include "ProtoEntityDefaultDefinitionFactory.h"

namespace applib {
using namespace std;
using namespace utils;
using namespace wp;

ProtoEntityDefaultDefinitionFactory::ProtoEntityDefaultDefinitionFactory()
    : ProtoEntityResourceDefinitionFactory("") {
}

vector<string> ProtoEntityDefaultDefinitionFactory::getExtraPropertyNames() const {
  return {};
}

void ProtoEntityDefaultDefinitionFactory::createProtoEntity(ProtoEntity* entity, wp::application::resourcesystem::ResourceManager* resourceMgr, wp::DataNode* node) {
  auto handler = getEntityHandler(entity);
  auto protoId = entity->getComponentSystemId();
  auto const& protoName = entity->getName();
  auto propNode = node->getChild("Properties");
  auto propertyNames = getExtraPropertyNames();
  propertyNames.push_back("Physical");
  propertyNames.push_back("Visual");
  propNode->requireOnlyChildren(propertyNames);

  // Physical
  auto physicalNode = propNode->getOptionalChild("Physical");
  if (physicalNode) {
    // Parse
    Vector2 position = Vector2::ZERO;
    float feetElevation = 0.0f;
    float angle = 0.0f;
    float pitch = 0.0f;

    bool collides = false;
    auto collisionNode = physicalNode->getOptionalChild("Collision");
    if (collisionNode) {
      collides = utils::StringUtils::parseBool(collisionNode->getValue());
    }

    Vector2 size(0, 0);

    if (collides) {
      auto sizeNode = physicalNode->getChild("Size");
      auto collType = sizeNode->getProperty("type");

      if (collType == "AABB") {
        auto sizeValue = sizeNode->getValue();
        auto sizeComponents = utils::StringUtils::split(sizeValue, ", ");

        size.x = utils::StringUtils::parseFloat(sizeComponents[0]);
        size.y = utils::StringUtils::parseFloat(sizeComponents[1]);
      } else {
        throw Exception("Unsupported collision size type: " + collType);
      }
    }

    BoundingBox bounds(position - size / 2, size);

    handler->registerProtoComponent<PhysicalStats>(protoId, {position,
                                                             feetElevation,
                                                             angle,
                                                             pitch,
                                                             collides,
                                                             bounds});
  }

  // Visual
  auto visualNode = propNode->getOptionalChild("Visual");
  if (visualNode) {
    auto animationResource = static_cast<application::resourcesystem::AnimationSetResource*>(entity->getDependentResource("Animations").get());
    auto animNode = visualNode->getOptionalChild("Animation");
    if (animNode) {
      auto animDatabase = getAnimationDatabase(entity);

      VisualSprite visual{-1, 0, 0, 0.0f};
      visual.animations.fill((uint32_t)-1);

      uint32_t initialAnimId{(uint32_t)-1};
      do {
        auto animKey = animNode->getProperty("key");
        auto animName = animNode->getValue();

        // Register animation
        auto animId = animDatabase->registerAnimation(
            animationResource->getName(),
            animationResource->getNamespace(),
            animName);

        // Start with the first listed animation
        if (initialAnimId == (uint32_t)-1) {
          initialAnimId = animId;
        }

        auto lookupKey = getAnimationIdFromName(protoName, animKey);
        if (lookupKey >= AnimationDatabase::MaxObjectAnimations) {
          throw Exception(std::format("Animation key {} out of bounds", lookupKey));
        }

        visual.animations[lookupKey] = animId;
      } while (animNode->next());

      visual.animation = initialAnimId;
      handler->registerProtoComponent<VisualSprite>(protoId, visual);
    }
  }
}

}  // namespace applib
