#include <willpower/application/resourcesystem/AnimationSetResource.h>
#include <willpower/application/resourcesystem/ImageResource.h>

#include "AnimationDatabase.h"
#include "Exceptions.h"

namespace applib {
using namespace std;
using namespace wp;

AnimationDatabase::AnimationDatabase(application::resourcesystem::ResourceManager* resourceMgr)
    : mwResourceMgr(resourceMgr) {
}

AnimationDatabase::~AnimationDatabase() {
  for (auto item : mFrameFunctionFactories) {
    auto const& [name, factory] = item;
    delete factory;
  }
}

void AnimationDatabase::registerFrameFunctionFactory(FrameFunctionFactory* factory) {
  auto const& name = factory->getName();

  auto it = mFrameFunctionFactories.insert(make_pair(name, factory));
  if (!it.second) {
    throw Exception("Frame function factory '" + name + "' already registered in animation database.");
  }
}

uint32_t AnimationDatabase::registerAnimation(string const& resourceName, string const& resourceNamespace, string const& animationName) {
  auto resource = mwResourceMgr->getResource(resourceName, resourceNamespace);
  auto animSet = static_cast<application::resourcesystem::AnimationSetResource*>(resource.get());
  return registerAnimation(animSet, animationName);
}

uint32_t AnimationDatabase::registerAnimation(application::resourcesystem::AnimationSetResource* resource, string const& animationName) {
  auto const& animData = resource->getAnimation(animationName);

  // Set up entry
  Animation entry;

  entry.name = animationName;
  entry.offset = (int32_t)mFrames.size();
  entry.count = (int32_t)animData.frames.size();

  switch (animData.loopStyle) {
    case application::resourcesystem::AnimationSetResource::LoopStyle::Forwards:
      entry.style = LoopStyle::Forwards;
      break;

    case application::resourcesystem::AnimationSetResource::LoopStyle::Once:
      entry.style = LoopStyle::Once;
      break;

    case application::resourcesystem::AnimationSetResource::LoopStyle::PingPong:
      entry.style = LoopStyle::PingPong;
      break;

    default:
      throw Exception("Unknown loop style.  Must be one of Forwards|Once|PingPoing.");
  }

  auto id = (uint32_t)mEntries.size();
  mEntries.push_back(entry);

  // Add frames: calculate uv-coords from underlying image
  auto imageResource = resource->getImage();
  auto image = static_cast<application::resourcesystem::ImageResource*>(imageResource.get());

  auto imageWidth = (float)image->getWidth();
  auto imageHeight = (float)image->getHeight();
  for (auto const& animFrame : animData.frames) {
    Frame frame;

    for (int i = 0; i < 2; ++i) {
      frame.u[i] = animFrame.u[i];
      frame.v[i] = animFrame.v[i];
    }

    frame.width = animFrame.width;
    frame.height = animFrame.height;

    frame.xoff = animFrame.renderOffsetX;
    frame.yoff = animFrame.renderOffsetY;

    frame.time = animFrame.time;

    // TODO: convert tags to callable functions
    for (auto item : animFrame.tags) {
      auto const& [funcName, args] = item;
    }

    mFrames.push_back(frame);
  }

  return id;
}

AnimationDatabase::Animation const& AnimationDatabase::getAnimation(uint32_t id) const {
  return mEntries.at(id);
}

int32_t AnimationDatabase::getAnimationFrameCount(uint32_t id) const {
  return mEntries.at(id).count;
}

AnimationDatabase::Frame const& AnimationDatabase::getAnimationFrame(uint32_t id, uint32_t index) const {
  auto const& animation = mEntries.at(id);
  if (animation.count <= 0 || index >= static_cast<uint32_t>(animation.count)) {
    throw Exception("Animation frame index is out of bounds.");
  }
  return mFrames.at(static_cast<size_t>(animation.offset) + index);
}

}  // namespace applib