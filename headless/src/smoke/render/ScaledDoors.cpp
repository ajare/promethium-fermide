#include "Checks.h"
#include "Render.h"
#include "WorldDrawList.h"
#include "UISettings.h"
#include "core/World.h"
extern UISettings gUISettings;
namespace {
void commands(smoke::Context const&) {
    using smoke::require;
    gUISettings.worldViewportHeight = 800; gUISettings.worldZoom = 1;
    gUISettings.xOffset = gUISettings.yOffset = 0;
    gUISettings.worldViewportX = gUISettings.worldViewportY = 0;
    core::World world("Scaled artwork", 8, 2);
    world.addRoom("Front", 0, 0, 0, 8, 1); world.addRoom("Back", 1, 0, 0, 8, 1);
    core::World::CreateDoorOptions o; o.heightScale = 0.5f;
    auto created = world.addSectorDoor(0, 0, 2, o); world.finishBuild();
    auto door = std::static_pointer_cast<core::DoorSectorObject>(created.door.sector->getObject(created.door.index))->getDoor();
    for (auto style : {core::Door::OpenStyle::OpenUp, core::Door::OpenStyle::OpenLeft,
        core::Door::OpenStyle::OpenRight, core::Door::OpenStyle::OpenApart}) {
        door->setOpenStyle(style);
        WorldDrawList list({{0,0},{1200,800}});
        renderDoor(door, 0, LayerRenderStyle::Solid, true, &list);
        require(!list.commands().empty(), "Scaled Door emitted no artwork");
        float top = 800 - 0.25f * CORE_LEVEL_HEIGHT_PIXELS;
        for (auto const& command : list.commands()) {
            if (auto t = std::get_if<WorldDrawList::Triangle>(&command); t &&
                (t->texture == WorldDrawList::Texture::ObjectAtlas ||
                 t->colour == IM_COL32(64,192,255,255) || t->colour == IM_COL32(255,255,0,48)))
                for (auto p : t->positions) require(p.y >= top - 0.001f && p.y <= 800.001f, "Door artwork not scaled bottom-anchored");
        }
    }
}
}
void render_smoke::registerScaledDoors(std::vector<smoke::Check>& checks) {
    checks.push_back({"scaled-doors/commands", commands});
}
