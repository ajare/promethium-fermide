#include "Checks.h"
#include "Render.h"
#include "WorldDrawList.h"
#include "UISettings.h"
#include "core/World.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"

#include <cmath>
#include <memory>
#include <variant>

extern UISettings gUISettings;

namespace {
	// A height-scaled Door keeps its leaf artwork inside the doorway it
	// actually fills, standing on the cell floor. Under the full-cell leaf
	// model (#1056) that is the clip's job rather than the vertex's: the
	// leaf is drawn as a whole cell and the scaled doorway is what cuts
	// it, so the check reads the triangle's clip instead of the extent of
	// the triangle itself.
	//
	// This pins the leaf against the scaled aperture and nothing else. The
	// frame the same Door would wear is pinned against the same aperture in
	// `render/DoorframeCells.cpp` (#1062), which is where it belongs: the
	// frame's hole is baked at the unscaled height, so a scaled Door wears
	// no frame at all. Each half used to be pinned at its own scale without
	// ever being compared with the other, and that is how the see-through
	// band between them shipped.
	float const kHeightScale = 0.5f;

	void commands(smoke::Context const&) {
	    using smoke::require;
	    gUISettings.worldViewportHeight = 800; gUISettings.worldZoom = 1;
	    gUISettings.xOffset = gUISettings.yOffset = 0;
	    gUISettings.worldViewportX = gUISettings.worldViewportY = 0;
	    core::World world("Scaled artwork", 8, 2);
	    world.addRoom("Front", 0, 0, 0, 8, 1); world.addRoom("Back", 1, 0, 0, 8, 1);
	    core::World::CreateDoorOptions o; o.heightScale = kHeightScale;
	    auto created = world.addSectorDoor(0, 0, 2, o); world.finishBuild();
	    auto door = std::static_pointer_cast<core::DoorSectorObject>(created.door.sector->getObject(created.door.index))->getDoor();

	    // The scaled doorway in screen pixels, read off the Door's own shape
	    // so the check follows whatever the scale resolved to.
	    core::Vector2 lo, hi;
	    door->getFullShape(lo, hi);
	    float const jambTop = 800.0f - std::max(lo.y, hi.y) * CORE_LEVEL_HEIGHT_PIXELS;
	    float const floorLine = 800.0f - std::min(lo.y, hi.y) * CORE_LEVEL_HEIGHT_PIXELS;
	    require(std::abs((floorLine - jambTop)
	            - kHeightScale * CORE_DOOR_HEIGHT * CORE_LEVEL_HEIGHT_PIXELS) < 0.001f,
	        "The scaled doorway is not the scaled Door height standing on the floor");

	    auto const isLeaf = [](WorldDrawList::Triangle const& t) {
	        return t.texture == WorldDrawList::Texture::ObjectAtlas
	            || t.colour == packColour(64, 192, 255, 255);
	    };

	    for (auto style : {core::Door::OpenStyle::OpenUp, core::Door::OpenStyle::OpenLeft,
	        core::Door::OpenStyle::OpenRight, core::Door::OpenStyle::OpenApart}) {
	        door->setOpenStyle(style);
	        WorldDrawList list({{0,0},{1200,800}});
	        renderDoor(door, 0, LayerRenderStyle::Solid, true, &list);
	        require(!list.commands().empty(), "Scaled Door emitted no artwork");
	        int leafTriangles = 0;
	        for (auto const& command : list.commands()) {
	            auto const* t = std::get_if<WorldDrawList::Triangle>(&command);
	            if (!t || !isLeaf(*t)) continue;
	            ++leafTriangles;
	            // The leaf reaches above the jamb as a whole cell; the clip is
	            // the scaled doorway, so nothing paints outside it.
	            require(t->clip.minimum.y >= jambTop - 0.001f
	                    && t->clip.maximum.y <= floorLine + 0.001f,
	                "Scaled Door leaf artwork is not clipped to the scaled doorway");
	            // Bottom-anchored: the clip stands on the cell floor and
	            // stops at the scaled jamb, so the visible artwork is
	            // anchored at the floor rather than floating.
	            require(std::abs(t->clip.maximum.y - floorLine) < 0.001f,
	                "Scaled Door leaf clip does not stand on the cell floor");
	            require(std::abs(t->clip.minimum.y - jambTop) < 0.001f,
	                "Scaled Door leaf clip does not reach the scaled jamb");
	            require(std::abs(t->clip.minimum.x - lo.x * CORE_CELL_WIDTH_PIXELS) < 0.001f
	                    && std::abs(t->clip.maximum.x - hi.x * CORE_CELL_WIDTH_PIXELS) < 0.001f,
	                "Scaled Door leaf clip does not span the doorway");
	        }
	        require(leafTriangles > 0, "Scaled Door emitted no leaf artwork");
	    }
	}
}

void render_smoke::registerScaledDoors(std::vector<smoke::Check>& checks) {
    checks.push_back({"scaled-doors/commands", commands});
}
