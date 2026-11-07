#include <willpower/common/Globals.h>

#include "VisualSpriteDataProvider.h"
#include "EntityFacade.h"
#include "Entity.h"
#include "ModelInstance.h"

namespace applib {
using namespace std;

VisualSpriteDataProvider::VisualSpriteDataProvider(EntityFacade* facade, shared_ptr<AnimationDatabase> animationDatabase)
    : mwFacade(facade), mAnimationDatabase(animationDatabase) {
  setNumPrimitives(0);
}

void VisualSpriteDataProvider::getBounds(glm::vec3& bMin, glm::vec3& bMax) {
  bMin.x = bMin.y = bMin.z = -1e10f;
  bMax.x = bMax.y = bMax.z = 1e10f;
}

void VisualSpriteDataProvider::position(uint32_t index, float& x, float& y) {
  auto const& entity = mwFacade->getEntity(index);
  auto const& visual = ModelInstance::entityHandler()->getEntityComponent<VisualSprite>(entity);
  auto const& physicalStats = ModelInstance::entityHandler()->getEntityComponent<PhysicalStats>(entity);

  auto const& frame = mAnimationDatabase->getAnimationFrame(
      (uint32_t)visual.animation,
      visual.frame);

  x = physicalStats.position.x + frame.xoff;
  y = physicalStats.position.y + frame.yoff;
}

void VisualSpriteDataProvider::angle(uint32_t index, float& angle) {
  auto const& entity = mwFacade->getEntity(index);
  auto const& physicalStats = ModelInstance::entityHandler()->getEntityComponent<PhysicalStats>(entity);

  angle = physicalStats.angle;
}

void VisualSpriteDataProvider::direction(uint32_t index, float& x, float& y) {
  x = 0;
  y = 1;
}

void VisualSpriteDataProvider::textureAtlasTexcoords(uint32_t index, float& u0, float& v0, float& u1, float& v1) {
  auto const& entity = mwFacade->getEntity(index);
  auto const& visual = ModelInstance::entityHandler()->getEntityComponent<VisualSprite>(entity);

  auto const& frame = mAnimationDatabase->getAnimationFrame(
      (uint32_t)visual.animation,
      visual.frame);

  u0 = frame.u[0];
  v0 = frame.v[0];
  u1 = frame.u[1];
  v1 = frame.v[1];
}

void VisualSpriteDataProvider::dimensions(uint32_t index, float& halfWidth, float& halfHeight) {
  auto const& entity = mwFacade->getEntity(index);
  auto const& visual = ModelInstance::entityHandler()->getEntityComponent<VisualSprite>(entity);

  auto const& frame = mAnimationDatabase->getAnimationFrame(
      (uint32_t)visual.animation,
      visual.frame);

  halfWidth = frame.width / 2.0f;
  halfHeight = frame.height / 2.0f;
}

mpp::Colour VisualSpriteDataProvider::diffuse() {
  return mpp::Colour::White;
}

bool VisualSpriteDataProvider::update(float frameTime) {
  setNumPrimitives(mwFacade->getCount());
  return true;
}

}  // namespace applib