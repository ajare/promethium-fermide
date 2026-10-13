#pragma once

#include <memory>

#include <willpower/application/resourcesystem/ResourceManager.h>

#include <willpower/collide/Simulation.h>

#include "Platform.h"
#include "EntityHandler.h"
#include "AnimationDatabase.h"

namespace applib {

typedef std::function<EntityHandler*(std::shared_ptr<AnimationDatabase>)> EntityHandlerFactoryFunction;

struct Model {
  std::shared_ptr<AnimationDatabase> animationDatabase;

  std::shared_ptr<EntityHandler> entityHandler;

  wp::collide::Simulation* collisionSim;

public:
  Model(EntityHandlerFactoryFunction handlerFactory, wp::application::resourcesystem::ResourceManager* resourceMgr)
      : collisionSim(nullptr) {
    animationDatabase = std::make_shared<AnimationDatabase>(resourceMgr);

    auto entityHandlerPtr = handlerFactory(animationDatabase);
    entityHandler = std::shared_ptr<EntityHandler>(entityHandlerPtr);
  }

  virtual ~Model() = default;
};

}  // namespace applib