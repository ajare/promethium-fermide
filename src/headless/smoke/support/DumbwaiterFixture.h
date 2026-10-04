#pragma once
#include "core/World.h"
#include "core/YamlSerializer.h"
#include "core/Button.h"

namespace dumbwaiter_fixture
{
	inline std::shared_ptr<core::World> make(unsigned kind = 0, bool shared = true, uint32_t shaftLayer = 1)
	{
		auto world = std::make_shared<core::World>("Dumbwaiter", 6, 4);
		while (world->getLayerCount() <= shaftLayer) world->addLayer();
		if (shared && kind != 1)
		{
			auto location = kind == 2 ? world->addFacade(shaftLayer - 1, 0, 2, 1, 2)
				: world->addRoom("Landing", shaftLayer - 1, 0, 2, 1, 2);
			world->addSectorWalkway(location, 1, 0);
		}
		else for (uint32_t y = 0; y < 2; ++y)
		{
			if (kind == 1) world->addCorridor(shaftLayer - 1, y, 2, 1, 1);
			else if (kind == 2) world->addFacade(shaftLayer - 1, y, 2, 1, 1);
			else world->addRoom("Landing", shaftLayer - 1, y, 2, 1, 1);
		}
		world->finishBuild(); world->pauseSimulation();
		return world;
	}
	inline void addLandings(core::World& world, uint32_t shaftLayer, uint32_t x, uint32_t y = 0, unsigned kind = 0)
	{
		while (world.getLayerCount() <= shaftLayer) world.addLayer();
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			if (kind == 1) world.addCorridor(shaftLayer - 1, y + stop, x, 1, 1);
			else if (kind == 2) world.addFacade(shaftLayer - 1, y + stop, x, 1, 1);
			else world.addRoom("Destination landing", shaftLayer - 1, y + stop, x, 1, 1);
		}
	}
	inline std::shared_ptr<const core::SectorObject> control(core::World const& world,
		core::DumbwaiterId id, uint32_t stop)
	{
		auto unit = world.lookupDumbwaiter(id);
		if (!unit) return nullptr;
		auto landing = unit->getStop(stop).sector;
		for (uint32_t i = 0; i < landing->getNumObjects(); ++i)
		{
			auto object = landing->getObject(i);
			auto button = object ? std::dynamic_pointer_cast<const core::Button>(object->_getObject()) : nullptr;
			if (button && button->getInteractionPointId() == unit->getLandingButton(stop)) return object;
		}
		return nullptr;
	}
	inline std::shared_ptr<core::World> oppositeLandings()
	{
		auto world = std::make_shared<core::World>("Independent landings", 6, 4);
		world->addRoom("Lower", 0, 0, 1, 3, 1);
		world->addRoom("Upper", 0, 1, 1, 2, 1);
		world->finishBuild(); world->pauseSimulation();
		return world;
	}
	inline std::string yaml(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
		work.markSerializedUnmodified = false; world.serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}
}
