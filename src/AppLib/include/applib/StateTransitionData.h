#pragma once

#include "MapTransitionData.h"

namespace applib {

struct StateTransitionData {
  std::string prevStateName;
  MapTransitionData mapData;
  void* userData{nullptr};
};

}  // namespace applib