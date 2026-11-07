#include <iostream>
#include <stdexcept>
#include <string>

#include "applib/EntityHandler.h"

namespace {

struct AlphaComponent {
  int value;
};

struct BetaComponent {
  int value;
};

class TwoPrototypeEntityHandler final : public applib::EntityHandler {
  std::string getPrototypeName(int type) override {
    switch (type) {
      case 1:
        return "Alpha";
      case 2:
        return "Beta";
      default:
        throw std::runtime_error("Unknown test entity type");
    }
  }

  void setupImpl(applib::Entity*) override {}
  void destroyImpl(applib::Entity*) override {}
  bool updateImpl(applib::Entity*, bool, float) override { return true; }
};

void require(bool condition, char const* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void copiesOnlyPrototypeComponents() {
  TwoPrototypeEntityHandler handler;
  auto const alphaPrototype = handler.registerPrototype("Alpha");
  auto const betaPrototype = handler.registerPrototype("Beta");
  handler.registerProtoComponent(alphaPrototype, AlphaComponent{11});
  handler.registerProtoComponent(betaPrototype, BetaComponent{22});

  applib::Entity alpha;
  applib::Entity beta;
  handler.setup(&alpha, 1, {}, 0.0f);
  handler.setup(&beta, 2, {}, 0.0f);

  require(handler.entityHasComponent<AlphaComponent>(alpha),
          "Alpha entity did not receive its prototype component");
  require(!handler.entityHasComponent<BetaComponent>(alpha),
          "Alpha entity received a component owned only by Beta");
  require(handler.getEntityComponent<AlphaComponent>(alpha).value == 11,
          "Alpha entity component was not copied from its prototype");

  require(handler.entityHasComponent<BetaComponent>(beta),
          "Beta entity did not receive its prototype component");
  require(!handler.entityHasComponent<AlphaComponent>(beta),
          "Beta entity received a component owned only by Alpha");
  require(handler.getEntityComponent<BetaComponent>(beta).value == 22,
          "Beta entity component was not copied from its prototype");

  handler.destroy(&alpha);
  handler.destroy(&beta);
}

}  // namespace

int main() {
  try {
    copiesOnlyPrototypeComponents();
    std::cout << "Entity prototype component-copy regression passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
