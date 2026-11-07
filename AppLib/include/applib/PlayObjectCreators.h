#pragma once

#include <willpower/application/resourcesystem/ResourceManager.h>

#include <willpower/collide/Simulation.h>

#include <willpower/viz/GeometryMeshRenderer.h>

#include "Platform.h"
#include "StateMapTransition.h"
#include "GeometryMeshRendererFactory.h"

namespace applib {

wp::viz::GeometryMeshRenderer* createMapRenderer(wp::application::resourcesystem::ResourcePtr resource, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, GeometryMeshRendererFactory* factory, bool useThreading);

void destroyMapRenderer(wp::viz::GeometryMeshRenderer* mapRenderer, bool useThreading);

wp::collide::Simulation* createMapCollisionSim(wp::application::resourcesystem::ResourcePtr resource, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, bool useThreading);

void destroyMapCollisionSim(wp::collide::Simulation* sim, bool useThreading);

}  // namespace applib
