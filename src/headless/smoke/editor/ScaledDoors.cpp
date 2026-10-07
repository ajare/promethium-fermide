#include "Checks.h"
#include "State.h"
#include "DocumentEdit.h"
#include "DoorClipboard.h"
#include "core/YamlSerializer.h"
namespace {
void clipboardHistory(smoke::Context const&) {
    editor_smoke::State state; using smoke::require;
    auto world = std::make_shared<core::World>("Clipboard", 8, 2);
    world->addRoom("Front", 0, 0, 0, 8, 1); world->addRoom("Back", 1, 0, 0, 8, 1); world->finishBuild();
    world->pauseSimulation(); gWorldDocumentHistory.clear();
    core::World::CreateDoorOptions o; o.heightScale = 0.4f; o.speedOverride = 0.7f;
    o.openStyle = core::Door::OpenStyle::OpenApart;
    YAML::Emitter output; writeDoorClipboardObject(output, o);
    auto node = YAML::Load(output.c_str()); auto pasted = readDoorClipboardObject(node);
    require(pasted.heightScale == o.heightScale && pasted.speedOverride == o.speedOverride && pasted.openStyle == o.openStyle, "Clipboard lost Door properties");
    uint32_t x = 2; std::string diagnostic;
    require(prepareDoorClipboardPaste(*world, 0, 0, x, pasted, diagnostic), "Paste refused: " + diagnostic);
    auto before = captureDocumentSnapshot(world);
    world->addSectorDoor(0, 0, x, pasted); world->finishBuild(); commitDocumentEdit(std::move(before));
    auto restore = [&](DocumentSnapshot const& snapshot) {
        auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize(); core::SerializationWorkData work;
        bool ok = world->deserialize(*reader, work); world->pauseSimulation(); return ok;
    };
    require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore), "Door undo failed");
    require(!world->getSectorDoorOptions(0, 0, 2, 1, pasted), "Undo retained Door");
    require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore), "Door redo failed");
    require(world->getSectorDoorOptions(0, 0, 2, 1, pasted) && pasted.heightScale == o.heightScale, "Redo lost scale");
    for (auto invalid : {0.f, 1.01f}) {
        node["heightScale"] = invalid; bool refused = false;
        try { readDoorClipboardObject(node); } catch (std::exception const&) { refused = true; }
        require(refused, "Invalid clipboard scale accepted");
    }
}
}
void editor_smoke::registerScaledDoors(std::vector<smoke::Check>& checks) {
    checks.push_back({"scaled-doors/clipboard-history", clipboardHistory});
}
