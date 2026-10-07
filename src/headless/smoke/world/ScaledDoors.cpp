#include "Checks.h"
#include "core/World.h"
#include "core/DoorSectorObject.h"
#include "core/YamlSerializer.h"
#include <cmath>

namespace {
void scaledDoors(smoke::Context const&) {
    using smoke::require;
    for (auto scale : {0.1f, 0.5f, 1.0f}) {
        core::World world("Scaled Doors", 8, 2);
        world.addRoom("Front", 0, 0, 0, 8, 1); world.addRoom("Back", 1, 0, 0, 8, 1);
        core::World::CreateDoorOptions o; o.heightScale = scale;
        auto created = world.addSectorDoor(0, 0, 2, o); world.finishBuild();
        auto door = std::static_pointer_cast<core::DoorSectorObject>(created.door.sector->getObject(created.door.index))->getDoor();
        require(door->getHeightScale() == o.heightScale, "Door scale not applied");
        auto writer = core::YamlSerializer::toString(); core::SerializationWorkData saveWork; world.serialize(*writer, saveWork); writer->serialize();
        auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
        core::World loaded("Loaded", 8, 2); core::SerializationWorkData work;
        require(loaded.deserialize(*reader, work), "Scaled Door load failed");
        core::World::CreateDoorOptions restored;
        require(loaded.getSectorDoorOptions(0, 0, 2, 1, restored) && restored.heightScale == o.heightScale, "Scale lost on save/load");
        world.resetSimulation();
        require(world.getSectorDoorOptions(0, 0, 2, 1, restored) && restored.heightScale == o.heightScale, "Scale lost on replay");
        for (auto invalid : {0.f, 1.01f, std::nanf("")}) {
            o.heightScale = invalid;
            require(!world.canAddCorridorDoor(0, 0, 4, o), "Invalid scale accepted");
        }
    }
}
}
void registerScaledDoors(std::vector<smoke::Check>& checks) {
    checks.push_back({"scaled-doors", scaledDoors});
}
