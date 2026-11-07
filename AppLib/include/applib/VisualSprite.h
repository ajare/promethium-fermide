#pragma once

#include <array>

#include "Platform.h"

#include "AnimationDatabase.h"

namespace applib {

struct VisualSprite {
  int animation;
  int direction;
  int frame;
  float timer;
  std::array<uint32_t, AnimationDatabase::MaxObjectAnimations> animations;
};

}  // namespace applib