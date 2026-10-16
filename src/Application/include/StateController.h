#pragma once

#include <applib/StateController.h>

#include "MapResources.h"

namespace pf::application {

class StateController final : public applib::StateController {
  std::unique_ptr<MapResources> mResources;
  void teardown() override;
  std::string getNextStateName(std::string const& previous,
                               applib::StateTransitionData*) override;
  void updateTransitionData(std::string const&, std::string const&,
                            applib::StateTransitionData*) override;

public:
  StateController(wp::application::resourcesystem::ResourceManager* resourceMgr,
                  wp::Logger* logger);
};

}  // namespace pf::application
