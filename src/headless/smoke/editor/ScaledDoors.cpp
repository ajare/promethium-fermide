#include "Checks.h"
#include "State.h"
#include "DocumentEdit.h"
#include "DoorClipboard.h"
#include "core/YamlSerializer.h"
#include "core/Agent.h"
#include "core/DoorEdge.h"
#include "core/Environment.h"
#include <cstdlib>
#include <iostream>
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
void crossingHistory(smoke::Context const&) {
    editor_smoke::State state; using smoke::require;
    auto world = std::make_shared<core::World>("Crossing edits", 8, 2);
    auto front = world->addRoom("Front", 0, 0, 0, 8, 1);
    auto back = world->addRoom("Back", 1, 0, 0, 8, 1);
    core::World::CreateDoorOptions options; options.heightScale = .8f;
    world->addSectorDoor(0, 0, 2, options);
    world->addSectorMarker(back, 0, 3.5f, "Goal"); world->finishBuild();
    auto id = world->createAgent("Traveller", front, 0, 2.5f);
    world->pauseSimulation(); gWorldDocumentHistory.clear();
    require(world->setAgentIndividualHeightModifier(id, .8f), "Initial Height edit failed");
    auto before = captureDocumentSnapshot(world);
    require(world->setSectorDoorHeightScale(0, 0, 2, 1, .85f), "Accepted height edit failed");
    commitDocumentEdit(std::move(before));
    require(gWorldDocumentHistory.undoCount() == 1, "Accepted height edit missing history");
    world->markSaved(); gWorldDocumentHistory.markSaved();
    require(world->resumeSimulation(), "Crossing resume failed");
    require(world->moveAgentToMarker(id, world->getMarkerIds().front()).accepted(), "Crossing goal failed");
    auto agent = world->lookupAgent(id).entity;
    bool crossing = false;
    for (unsigned tick = 0; tick < 600; ++tick) {
        world->advanceTick();
        if (agent->getState() == core::Agent::State::TraversingEdge) { crossing = true; break; }
    }
    require(crossing, "No active Door crossing observed");
    std::string reason;
    require(!world->setSectorDoorHeightScale(0, 0, 2, 1, .6f, &reason), "Unpaused edit accepted");
    auto position = agent->getGlobalPosition();
    world->pauseSimulation();
    require(agent->getGlobalPosition() == position && agent->getState() == core::Agent::State::TraversingEdge,
        "Pause cancelled or teleported admitted crossing");
    auto snapshot = captureDocumentSnapshot(world);
    auto graph = world->getGraph(); auto dirty = world->isModified();
    auto historyDirty = gWorldDocumentHistory.isModified();
    require(!world->setSectorDoorHeightScale(0, 0, 2, 1, .6f, &reason)
        && reason.find("active crossing") != std::string::npos, "Paused crossing scale edit accepted");
    require(!world->setSectorDoorHeight(0, 0, 2, 1, core::Door::Height::Tall, &reason), "Paused crossing height edit accepted");
    require(captureDocumentSnapshot(world)->yaml == snapshot->yaml && world->getGraph() == graph
        && world->isModified() == dirty && gWorldDocumentHistory.isModified() == historyDirty
        && gWorldDocumentHistory.undoCount() == 1, "Refused edit mutated authored state/topology/history");
    require(world->setAgentIndividualHeightModifier(id, 1.f), "Admitted envelope edit failed");
    require(world->resumeSimulation(), "Admitted crossing resume failed");
    for (unsigned tick = 0; tick < 1200 && agent->getState() != core::Agent::State::Idle; ++tick) world->advanceTick();
    require(agent->getSector() == world->getSector(back).get() && agent->getState() == core::Agent::State::Idle,
        "Admitted enlarged Agent did not finish crossing");
    world->pauseSimulation();
    auto restore = [&](DocumentSnapshot const& saved) {
        auto reader = core::YamlSerializer::fromString(saved.yaml); reader->deserialize(); core::SerializationWorkData work;
        bool ok = world->deserialize(*reader, work); world->pauseSimulation(); return ok;
    };
    auto clearance = [&] {
        for (auto const& edge : world->getGraph()->getEdges())
            if (auto door = std::dynamic_pointer_cast<core::DoorEdge const>(edge)) {
                require(door->getDoor()->getSize().y == core::Door::effectiveHeight(core::Door::Height::Regular, options.heightScale),
                    "History restored inconsistent Door geometry");
                return door->getDoor()->admitsVerticalExtent(.41f, 0.f);
            }
        throw std::runtime_error("History lost Door edge");
    };
    require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore), "Height edit undo failed");
    require(world->getSectorDoorOptions(0, 0, 2, 1, options) && options.heightScale == .8f,
        "Next undo did not undo previous accepted edit");
    require(!clearance(), "Undo retained enlarged clearance");
    require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore), "Height edit redo failed");
    require(world->getSectorDoorOptions(0, 0, 2, 1, options) && options.heightScale == .85f, "Height redo lost geometry");
    require(clearance(), "Redo failed to restore enlarged clearance");
    if (core::hasEnvironmentVariable("PF_DOOR_CLEARANCE_TRACE")) std::cout << "[live-clearance] editor outcome=unpaused-refusal/paused-atomic-refusal/admitted-enlargement-completed/undo-redo\n";
}
}
void editor_smoke::registerScaledDoors(std::vector<smoke::Check>& checks) {
    checks.push_back({"scaled-doors/clipboard-history", clipboardHistory});
    checks.push_back({"scaled-doors/crossing-history", crossingHistory});
}
