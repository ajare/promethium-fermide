#pragma once

#include <applib/StateController.h>

namespace pf::application {

class StateController final : public applib::StateController {
  std::string getNextStateName(std::string const& previous,
                               applib::StateTransitionData*) override;
  void updateTransitionData(std::string const&, std::string const&,
                            applib::StateTransitionData*) override;

public:
  StateController();
};

}  // namespace pf::application
