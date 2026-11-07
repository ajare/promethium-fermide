#pragma once

#include <vector>
#include <functional>

#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/application/resourcesystem/AnimationSetResource.h>

#include "Platform.h"
#include "Entity.h"

namespace applib {

class FrameFunctionFactory {
  std::string mName;

public:
  explicit FrameFunctionFactory(std::string const& name)
      : mName(name) {
  }

  std::string const& getName() const {
    return mName;
  }

  virtual std::function<void(Entity*)> create() = 0;
};

class APPLIB_API AnimationDatabase {
public:
  typedef std::function<void(Entity*)> FrameFunction;

public:
  enum class LoopStyle {
    Forwards,
    Once,
    PingPong
  };

public:
  struct Animation {
    std::string name;
    int32_t offset{-1};
    int32_t count{-1};
    LoopStyle style;
  };

  struct Frame {
    float u[2];
    float v[2];
    int width, height;
    int xoff, yoff;
    float time;
    std::vector<FrameFunction> funcs;
  };

public:
  static const size_t MaxObjectTypes = 1024;

  static const size_t MaxObjectAnimations = 32;

private:
  std::map<std::string, FrameFunctionFactory*> mFrameFunctionFactories;

  wp::application::resourcesystem::ResourceManager* mwResourceMgr;

  std::vector<Animation> mEntries;

  std::vector<Frame> mFrames;

public:
  explicit AnimationDatabase(wp::application::resourcesystem::ResourceManager* resourceMgr);

  virtual ~AnimationDatabase();

  void registerFrameFunctionFactory(FrameFunctionFactory* factory);

  uint32_t registerAnimation(std::string const& resourceName, std::string const& resourceNamespace, std::string const& animationName);

  uint32_t registerAnimation(wp::application::resourcesystem::AnimationSetResource* resource, std::string const& animationName);

  Animation const& getAnimation(uint32_t id) const;

  int32_t getAnimationFrameCount(uint32_t id) const;

  Frame const& getAnimationFrame(uint32_t id, uint32_t index) const;
};

}  // namespace applib