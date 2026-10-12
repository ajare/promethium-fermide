#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <willpower/application/StateExceptions.h>

#include "Application.h"

namespace {
void require(bool condition, char const* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
}

int main() {
  try {
    require(std::string_view(dllGetName()) == "Promethium Fermide",
            "Application name mismatch");
    require(dllSetArgument("Unknown", "true") != 0,
            "Unknown arguments must be rejected");
    require(!dllGetNextStateFactory(), "No factory before entry");

    for (int run = 0; run < 2; ++run) {
      dllOnEntry(nullptr, nullptr, nullptr);
      auto factory = dllGetNextStateFactory();
      require(factory && factory->getType() == "Play", "Play must be initial state");
      require(!dllGetNextStateFactory(), "Only one state factory expected");
      require(!dllGetNextStateFactory(), "Enumeration must stay exhausted");
      {
        std::unique_ptr<wp::application::State> state(factory->createState());
        require(state->getName() == "Play", "State name mismatch");
        require(!state->_imGuiActive(), "Play must not display a GUI");
        require(state->getDebuggingText().empty(), "Play must have no debug text");
        state->_injectKeyInput(wp::application::KeyEvent::Released,
                               wp::application::Key::Escape, {});
        state->_injectKeyInput(wp::application::KeyEvent::Pressed,
                               wp::application::Key::Space, {});
        bool returned = false;
        try {
          state->_injectKeyInput(wp::application::KeyEvent::Pressed,
                                 wp::application::Key::Escape, {});
        } catch (wp::application::ReturnFromStateException const&) {
          returned = true;
        }
        require(returned, "Escape must unwind the state stack");
      }
      dllOnExit();
      require(!dllGetNextStateFactory(), "No factory after exit");
      dllOnExit();
    }
    return 0;
  } catch (std::exception const& error) {
    dllOnExit();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
