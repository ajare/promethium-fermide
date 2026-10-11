#include <iostream>
#include <stdexcept>
#include <string>

#include "applib/ObjectArray.h"

namespace {

void require(bool condition, std::string const& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void emptyArrayAcquiresAndGrows() {
  applib::ObjectArray<int> objects(0);

  auto const firstSlot = objects.acquireFreeSlot();
  *objects.getObject(firstSlot) = 42;
  require(*objects.getObject(firstSlot) == 42,
          "the first slot acquired from an empty ObjectArray is not usable");

  for (int i = 0; i < 8; ++i) {
    auto const slot = objects.acquireFreeSlot();
    require(slot != firstSlot, "ObjectArray handed out an occupied slot");
  }

  objects.releaseSlot(firstSlot);
  require(objects.acquireFreeSlot() == firstSlot,
          "ObjectArray did not return a released slot");
}

}  // namespace

int main() {
  try {
    emptyArrayAcquiresAndGrows();
    std::cout << "ObjectArray empty-growth regression passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
